#include "__trace_internal.h"

#include <stdlib.h>

#include <zlib.h>
#include <openssl/aes.h>
#include <openssl/evp.h>

/*
 * Fair encryption key, chosen by random number generator :)
 *      https://xkcd.com/221/
 *
 * Jokes aside, in the future I'll implement some kind of TLS
 * mechanism for encrypting data being sent over these network
 * backends/frontends. For now though, a shared symmetric key
 * will work well enough for the few deployments of this toolkit
 * that we have.
 */
static const uint8_t socket_key[] = {
        0xb8, 0xc2, 0xfe, 0x5a,
        0x01, 0xe8, 0x4c, 0x5b,
        0xf6, 0x9a, 0xe0, 0x59,
        0x1f, 0x02, 0x82, 0x75
};

static const uint8_t socket_iv[] = {
        0x1a, 0x58, 0x92, 0x4e,
        0xfe, 0xf0, 0x2b, 0x6b,
        0x7b, 0x3d, 0x95, 0x33,
        0x5a, 0x2d, 0x45, 0x54
};

int __compress(void *data, size_t len, void **compressed, int *compressed_len)
{
    int ret;
    size_t bound;
    void *res;
    z_stream def_stream = {
            .zalloc = Z_NULL,
            .zfree = Z_NULL,
            .opaque = Z_NULL
    };

    def_stream.avail_in = len;
    def_stream.next_in = (Bytef *) data;
    deflateInit(&def_stream, Z_BEST_COMPRESSION);

    bound = deflateBound(&def_stream, len);
    res = calloc(bound, 1);
    if(!res)
    {
        err("Failed to allocate result buffer\n");
        return -ENOMEM;
    }

    def_stream.avail_out = bound;
    def_stream.next_out = (Bytef *) res;

    ret = deflate(&def_stream, Z_FINISH);
    if(ret != Z_STREAM_END)
    {
        err("zlib deflation error\n");
        free(res);
        return -EINVAL;
    }

    deflateEnd(&def_stream);

    *compressed = res;
    *compressed_len = (int) def_stream.total_out;
    return 0;
}

int __encrypt(void *data, int len, void **encrypted, int *encrypted_len)
{
    int c_len = len + AES_BLOCK_SIZE, f_len = 0;

    void *res = calloc(c_len, 1);
    if(!res)
    {
        err("Failed to allocate result buffer\n");
        return -ENOMEM;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(ctx);
    EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, socket_key, socket_iv);

    EVP_EncryptUpdate(ctx, res, &c_len, data, len);
    EVP_EncryptFinal_ex(ctx, res + c_len, &f_len);
    EVP_CIPHER_CTX_free(ctx);

    *encrypted = res;
    *encrypted_len = c_len + f_len;
    return 0;
}

int send_over_socket(void *data, size_t len, FILE *netfile)
{
    int ret, compressed_len, encrypted_len;
    void *compressed, *encrypted;
    size_t written;

    ret = __compress(data, len, &compressed, &compressed_len);
    if(ret < 0)
    {
        err("Failed to compress data\n");
        return ret;
    }

    ret = __encrypt(compressed, compressed_len, &encrypted, &encrypted_len);
    free(compressed);
    if(ret < 0)
    {
        err("Failed to encrypt data\n");
        return ret;
    }

    written = fwrite(&encrypted_len, sizeof(int), 1, netfile);
    if(written != 1)
    {
        err("Failed to send encrypted length over socket\n");
        free(encrypted);
        return -errno;
    }

    written = fwrite(encrypted, 1, encrypted_len, netfile);
    free(encrypted);
    if(written != encrypted_len)
    {
        err("Failed to send all encrypted bytes over socket\n");
        return -errno;
    }

    return 0;
}

int __decompress(void *data, size_t len, void *decompressed, size_t expecting_len)
{
    int ret;
    z_stream inf_stream = {
            .zalloc = Z_NULL,
            .zfree = Z_NULL,
            .opaque = Z_NULL
    };

    inf_stream.avail_in = len;
    inf_stream.next_in = (Bytef *) data;
    inf_stream.avail_out = expecting_len;
    inf_stream.next_out = (Bytef *) decompressed;

    inflateInit(&inf_stream);
    ret = inflate(&inf_stream, Z_FINISH);
    if(ret != Z_STREAM_END)
    {
        err("Zlib inflation error\n");
        return -EINVAL;
    }

    inflateEnd(&inf_stream);
    return 0;
}

int __decrypt(void *data, int len, void **decrypted, int *decrypted_len)
{
    int p_len = len, f_len = 0;
    void *res = calloc(p_len, 1);
    if(!res)
    {
        err("Failed to allocate result buffer\n");
        return -ENOMEM;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(ctx);
    EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, socket_key, socket_iv);

    EVP_DecryptUpdate(ctx, res, &p_len, data, len);
    EVP_DecryptFinal_ex(ctx, res + p_len, &f_len);
    EVP_CIPHER_CTX_free(ctx);

    *decrypted = res;
    *decrypted_len = p_len + f_len;
    return 0;
}

int recv_over_socket(void *data, size_t len, FILE *netfile)
{
    int ret, encrypted_len, compressed_len;
    void *encrypted, *compressed;

    size_t read = fread(&encrypted_len, sizeof(int), 1, netfile);
    if(read != 1)
    {
        err("Failed to receive encrypted length over socket\n");
        return -errno;
    }

    encrypted = calloc(encrypted_len, 1);
    if(!encrypted)
    {
        err("Failed to allocate result array\n");
        return -ENOMEM;
    }

    read = fread(encrypted, 1, encrypted_len, netfile);
    if(read != encrypted_len)
    {
        err("Failed to receive all encrypted bytes over socket\n");
        free(encrypted);
        return -errno;
    }

    ret = __decrypt(encrypted, encrypted_len, &compressed, &compressed_len);
    free(encrypted);
    if(ret < 0)
    {
        err("Failed to decrypt data\n");
        return ret;
    }

    ret = __decompress(compressed, compressed_len, data, len);
    free(compressed);
    if(ret < 0)
    {
        err("Failed to decompress data\n");
        return ret;
    }

    return 0;
}