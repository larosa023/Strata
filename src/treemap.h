// Strata - squarified treemap layout (Space view)
#pragma once
#include <vector>
#include <d2d1.h>

// `sizes` must be sorted largest first; returns one rectangle per size inside `area`.
std::vector<D2D1_RECT_F> Squarify(const std::vector<double>& sizes, D2D1_RECT_F area);
