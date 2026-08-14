// tq-dbr-tool: path normalization and variable printing shared by the
// command modules.

#include "tq_dbr_tool.h"

// Normalizes a path by lowercasing and converting forward slashes to
// backslashes (arz paths use backslashes).
// input: the path string to normalize.
// Returns a newly allocated normalized string (free with g_free).
char *
normalize_path(const char *input)
{
  char *out = g_ascii_strdown(input, -1);

  for(char *p = out; *p; p++)
    if(*p == '/')
      *p = '\\';

  return(out);
}

// Prints a single TQVariable's name and all its values to stdout.
// v: pointer to the variable to print.
void
print_variable(TQVariable *v)
{
  printf("  %-40s ", v->name);

  if(v->count == 0)
  {
    printf("(empty)\n");
    return;
  }

  for(uint32_t j = 0; j < v->count; j++)
  {
    if(v->type == TQ_VAR_INT)
    {
      if(v->value.i32)
        printf("%d", v->value.i32[j]);
      else
        printf("(null)");
    }
    else if(v->type == TQ_VAR_FLOAT)
    {
      if(v->value.f32)
        printf("%.4f", v->value.f32[j]);
      else
        printf("(null)");
    }
    else if(v->type == TQ_VAR_STRING)
    {
      if(v->value.str)
        printf("%s", v->value.str[j] ? v->value.str[j] : "(null)");
      else
        printf("(null)");
    }
    else
    {
      printf("(unknown type %d)", v->type);
    }

    if(j < v->count - 1)
      printf(", ");
  }

  printf("\n");
}
