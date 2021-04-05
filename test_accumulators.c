#include "statistics.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "transform.h"

int __plot_indices(struct viz_args *tfm, int r, int c, int i)
{
    if(tfm->fill_order[2] == PLOTS)
    {
        if(tfm->fill_order[0] == ROWS)
            return r + c + (tfm->rows * tfm->cols) * i;
        else if(tfm->fill_order[0] == COLS)
            return c * tfm->rows + r + (tfm->rows * tfm->cols) * i;
        else
        {
            // error
            return -1;
        }
    }
    else if(tfm->fill_order[0] == PLOTS)
    {
        if(tfm->fill_order[1] == ROWS)
            return r * (tfm->cols * tfm->plots) + tfm->plots * c + i;
        else if(tfm->fill_order[1] == COLS)
            return c * (tfm->rows * tfm->plots) + tfm->plots * r + i;
        else
        {
            // error
            return -1;
        }
    }
    else if(tfm->fill_order[1] == PLOTS)
    {
        if(tfm->fill_order[0] == ROWS)
            return r * (tfm->cols * tfm->plots) + c + (tfm->cols) * i;
        else if(tfm->fill_order[0] == COLS)
            return c * (tfm->rows * tfm->plots) + r + (tfm->rows) * i;
    }
}

int __plot_indices_rearranged(struct viz_args *tfm, int r, int c, int i)
{
    if(tfm->fill_order[0] == PLOTS)
    {
        if(tfm->fill_order[1] == ROWS)
            return r * (tfm->cols * tfm->plots) + tfm->plots * c + i;
        else if(tfm->fill_order[1] == COLS)
            return c * (tfm->rows * tfm->plots) + tfm->plots * r + i;
    }
    else if(tfm->fill_order[0] == ROWS)
    {
        if(tfm->fill_order[1] == PLOTS)
            return r * (tfm->cols * tfm->plots) + c + (tfm->cols) * i;
        else if(tfm->fill_order[1] == COLS)
            return r + c + (tfm->rows * tfm->cols) * i;
    }
    else if(tfm->fill_order[0] == COLS)
    {
        if(tfm->fill_order[1] == ROWS)
            return c * tfm->rows + r + (tfm->rows * tfm->cols) * i;
        else if(tfm->fill_order[1] == PLOTS)
            return c * (tfm->rows * tfm->plots) + r + (tfm->rows) * i;
    }

    return -1;
}

int main()
{
    struct viz_args tfm = {
            .rows = 4,
            .cols = 4,
            .plots = 256,

            .fill_order = {PLOTS, ROWS, COLS}
    };

    int i, j, k;
    for(i = 0; i < tfm.rows; i++)
    {
        fprintf(stderr, "row %i\n", i);
        for(j = 0; j < tfm.cols; j++)
        {
            fprintf(stderr, "\tcol %i\n\t\t", j);

            for(k = 0; k < tfm.plots; k++)
                fprintf(stderr, "%i,", __plot_indices_rearranged(&tfm, i, j, k));
            fprintf(stderr, "\n");
        }
    }
}