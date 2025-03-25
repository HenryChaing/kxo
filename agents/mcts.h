// #pragma once
#include <math.h>
#define ITERATIONS 100000
#define EXPLORATION_FACTOR sqrt(2)

int mcts(const char *table, char player);
