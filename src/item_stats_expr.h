#ifndef ITEM_STATS_EXPR_H
#define ITEM_STATS_EXPR_H

// Recursive-descent evaluator for the itemCost requirement equations
// (supports + - * / ^, parentheses, decimals, and the "itemLevel" /
// "totalAttCount" variables), plus the item-Class -> equation-prefix map
// used to build the per-slot equation field names.

double eval_equation(const char *eq, double item_level, double total_att_count);
const char *class_to_equation_prefix(const char *item_class);

#endif
