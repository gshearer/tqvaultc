#include "item_stats_expr.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

// Simple recursive-descent expression evaluator for itemCost equations.
// Supports: +, -, *, /, ^ (power), parentheses, decimal numbers, and
// variable substitution for "itemLevel" and "totalAttCount".

typedef struct {
  const char *p;
  double item_level;
  double total_att_count;
} ExprCtx;

static double expr_parse_expr(ExprCtx *c);

// Skip whitespace in the expression context.
// c: expression context.
static void
expr_skip_ws(ExprCtx *c)
{
  while(*c->p == ' ' || *c->p == '\t')
    c->p++;
}

// Parse an atom (number, variable, or parenthesized expression).
// c: expression context.
// Returns: parsed value.
static double
expr_parse_atom(ExprCtx *c)
{
  expr_skip_ws(c);

  if(*c->p == '(')
  {
    c->p++;

    double v = expr_parse_expr(c);

    expr_skip_ws(c);

    if(*c->p == ')')
      c->p++;

    return(v);
  }

  // variable or number
  if((*c->p >= 'a' && *c->p <= 'z') || (*c->p >= 'A' && *c->p <= 'Z'))
  {
    const char *start = c->p;

    while((*c->p >= 'a' && *c->p <= 'z') || (*c->p >= 'A' && *c->p <= 'Z') ||
           (*c->p >= '0' && *c->p <= '9') || *c->p == '_')
      c->p++;

    size_t len = (size_t)(c->p - start);

    if(len == 9 && strncmp(start, "itemLevel", 9) == 0)
      return(c->item_level);

    if(len == 13 && strncmp(start, "totalAttCount", 13) == 0)
      return(c->total_att_count);

    return(0.0);
  }

  // number (possibly negative handled by caller via unary minus)
  char *end;
  double v = strtod(c->p, &end);

  if(end == c->p)
    return(0.0);

  c->p = end;

  return(v);
}

// Parse a unary expression (handles leading +/-).
// c: expression context.
// Returns: parsed value.
static double
expr_parse_unary(ExprCtx *c)
{
  expr_skip_ws(c);

  if(*c->p == '-')
  {
    c->p++;
    return(-expr_parse_unary(c));
  }

  if(*c->p == '+')
  {
    c->p++;
    return(expr_parse_unary(c));
  }

  return(expr_parse_atom(c));
}

// Parse a power expression (right-associative ^).
// c: expression context.
// Returns: parsed value.
static double
expr_parse_power(ExprCtx *c)
{
  double v = expr_parse_unary(c);

  expr_skip_ws(c);

  if(*c->p == '^')
  {
    c->p++;
    v = pow(v, expr_parse_power(c));
  }

  return(v);
}

// Parse a multiplication/division expression.
// c: expression context.
// Returns: parsed value.
static double
expr_parse_muldiv(ExprCtx *c)
{
  double v = expr_parse_power(c);

  for(;;)
  {
    expr_skip_ws(c);

    if(*c->p == '*')
    {
      c->p++;
      v *= expr_parse_power(c);
    }

    else if(*c->p == '/')
    {
      c->p++;

      double d = expr_parse_power(c);

      if(d != 0)
        v /= d;
    }

    else
      break;
  }

  return(v);
}

// Parse an addition/subtraction expression (top-level).
// c: expression context.
// Returns: parsed value.
static double
expr_parse_expr(ExprCtx *c)
{
  double v = expr_parse_muldiv(c);

  for(;;)
  {
    expr_skip_ws(c);

    if(*c->p == '+')
    {
      c->p++;
      v += expr_parse_muldiv(c);
    }

    else if(*c->p == '-')
    {
      c->p++;
      v -= expr_parse_muldiv(c);
    }

    else
      break;
  }

  return(v);
}

// Evaluate a string equation with variable substitution.
// eq: equation string.
// item_level: value for "itemLevel" variable.
// total_att_count: value for "totalAttCount" variable.
// Returns: computed result.
double
eval_equation(const char *eq, double item_level, double total_att_count)
{
  ExprCtx c = { .p = eq, .item_level = item_level, .total_att_count = total_att_count };

  return(expr_parse_expr(&c));
}

// Map item Class to equation prefix used in itemCost records.
// item_class: Class string from DBR.
// Returns: equation prefix, or NULL if unknown.
const char *
class_to_equation_prefix(const char *item_class)
{
  if(!item_class)
    return(NULL);

  static const struct { const char *cls; const char *prefix; } map[] = {
    {"ArmorProtective_Head",          "head"},
    {"ArmorProtective_UpperBody",     "upperBody"},
    {"ArmorProtective_Forearm",       "forearm"},
    {"ArmorProtective_LowerBody",     "lowerBody"},
    {"ArmorJewelry_Ring",             "ring"},
    {"ArmorJewelry_Amulet",           "amulet"},
    {"WeaponHunting_Spear",           "spear"},
    {"WeaponMagical_Staff",           "staff"},
    {"WeaponHunting_RangedOneHand",   "bow"},
    {"WeaponHunting_Bow",             "bow"},
    {"WeaponMelee_Sword",             "sword"},
    {"WeaponMelee_Mace",              "mace"},
    {"WeaponMelee_Axe",               "axe"},
    {"WeaponArmor_Shield",            "shield"},
    {"ArmorJewelry_Bracelet",         "bracelet"},
    {NULL, NULL}
  };

  for(int i = 0; map[i].cls; i++)
  {
    if(strcasecmp(item_class, map[i].cls) == 0)
      return(map[i].prefix);
  }

  return(NULL);
}
