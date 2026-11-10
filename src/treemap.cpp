// Strata - squarified treemap layout (Bruls, Huizing & van Wijk)
#include "treemap.h"
#include <algorithm>
#include <cmath>

static double Worst(const std::vector<double>& row, double sum, double side) {
    if (row.empty() || sum <= 0 || side <= 0) return 1e300;
    double mx = *std::max_element(row.begin(), row.end()), mn = *std::min_element(row.begin(), row.end());
    double s2 = sum * sum, w2 = side * side;
    return std::max(w2 * mx / s2, s2 / (w2 * std::max(mn, 1e-12)));
}

std::vector<D2D1_RECT_F> Squarify(const std::vector<double>& sizesIn, D2D1_RECT_F area) {
    std::vector<D2D1_RECT_F> out(sizesIn.size(), D2D1::RectF(0, 0, 0, 0));
    double total = 0;
    for (double s : sizesIn) total += std::max(0.0, s);
    double W = area.right - area.left, H = area.bottom - area.top;
    if (total <= 0 || W <= 0 || H <= 0) return out;
    // scale sizes to areas
    std::vector<double> a(sizesIn.size());
    for (size_t i = 0; i < a.size(); ++i) a[i] = std::max(0.0, sizesIn[i]) * (W * H / total);
    double x = area.left, y = area.top, w = W, h = H;
    size_t i = 0;
    while (i < a.size()) {
        double side = std::min(w, h);
        std::vector<double> row;
        double sum = 0;
        size_t j = i;
        while (j < a.size()) {
            std::vector<double> next = row;
            next.push_back(a[j]);
            if (!row.empty() && Worst(next, sum + a[j], side) > Worst(row, sum, side)) break;
            row = std::move(next);
            sum += a[j];
            ++j;
        }
        if (sum <= 0) { i = j; continue; }
        // lay the row along the shorter side
        if (w >= h) {
            double rw = sum / h, yy = y;
            for (size_t k = i; k < j; ++k) {
                double rh = a[k] / rw;
                out[k] = D2D1::RectF((float)x, (float)yy, (float)(x + rw), (float)(yy + rh));
                yy += rh;
            }
            x += rw;
            w -= rw;
        } else {
            double rh = sum / w, xx = x;
            for (size_t k = i; k < j; ++k) {
                double rw = a[k] / rh;
                out[k] = D2D1::RectF((float)xx, (float)y, (float)(xx + rw), (float)(y + rh));
                xx += rw;
            }
            y += rh;
            h -= rh;
        }
        i = j;
    }
    return out;
}
