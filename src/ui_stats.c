// ui_stats.c -- Stat table building and update logic (extracted from ui.c)

#include "ui.h"
#include "arz.h"
#include "asset_lookup.h"
#include "item_stats.h"
#include "prefetch.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

// Extract a readable mastery name from a full DBR path.
// e.g. "Records/Skills/Masteries/MasteryWarfare.dbr" -> "Warfare"
// @param dbr_path  full path to a mastery DBR record
// @return pointer into a static buffer (not thread-safe)
static const char *
mastery_display_name(const char *dbr_path)
{
  static char buf[64];

  if(!dbr_path)
  {
    buf[0] = '\0';
    return(buf);
  }

  // find last path separator
  const char *base = dbr_path;

  for(const char *p = dbr_path; *p; p++)
    if(*p == '/' || *p == '\\')
      base = p + 1;

  // strip leading "Mastery" (case-insensitive)
  if(strncasecmp(base, "Mastery", 7) == 0)
    base += 7;

  // copy up to (but not including) the last '.'
  const char *dot = strrchr(base, '.');
  size_t len = dot ? (size_t)(dot - base) : strlen(base);

  // strip trailing "Mastery" (e.g. "EarthMastery" -> "Earth")
  if(len >= 7 && strncasecmp(base + len - 7, "Mastery", 7) == 0)
    len -= 7;

  if(len >= sizeof(buf))
    len = sizeof(buf) - 1;

  memcpy(buf, base, len);
  buf[len] = '\0';

  // Cosmetic: display "Rogue" instead of "Stealth" mastery name
  if(strcasecmp(buf, "Stealth") == 0)
    return("Rogue");

  return(buf);
}

// Update all resistance, damage, speed, health, and ability stat tables
// from the character's equipped items.
// @param widgets  application widget tree containing stat table cells
// @param chr      character whose equipment to read stats from
void
update_resist_damage_tables(AppWidgets *widgets, TQCharacter *chr)
{
  if(!chr)
    return;

  // Populate resistance table
  static const char *resist_attrs[9] = {
    "defensivePhysical", "defensivePierce", "defensivePoison", "defensiveBleeding",
    "defensiveLife", "defensiveElementalResistance", "defensiveFire", "defensiveCold",
    "defensiveLightning"
  };

  // equipment[] indices for each table row (same order as row_labels)
  static const int slot_indices[12] = { 7, 8, 9, 10, 5, 6, 1, 0, 2, 3, 4, 11 };

  float slot_vals[12][9];

  for(int r = 0; r < 12; r++)
  {
    int idx = slot_indices[r];

    for(int c = 0; c < 9; c++)
    {
      float val = 0.0f;

      if(chr->equipment[idx])
        val = item_get_guaranteed_stat(chr->equipment[idx], resist_attrs[c]);

      slot_vals[r][c] = val;
      GtkWidget *cell_w = widgets->resist_cells[r][c];
      gtk_widget_remove_css_class(cell_w, "resist-cell-zero");
      gtk_widget_remove_css_class(cell_w, "resist-cell-pos");
      gtk_widget_remove_css_class(cell_w, "resist-cell-high");

      if(val > 0.001f)
      {
        char cell[16];

        snprintf(cell, sizeof(cell), "+%d%%", (int)val);
        gtk_label_set_text(GTK_LABEL(cell_w), cell);
      }
      else
      {
        gtk_label_set_text(GTK_LABEL(cell_w), "\xe2\x80\x94");
        gtk_widget_add_css_class(cell_w, "resist-cell-zero");
      }
    }
  }

  // Total rows: row 12 = Primary (exclude AltRight row=2 and AltLeft row=3)
  //             row 13 = Alternate (exclude Right row=0 and Left row=1)
  for(int c = 0; c < 9; c++)
  {
    float total_p = 0.0f, total_a = 0.0f;

    for(int r = 0; r < 12; r++)
    {
      if(r != 2 && r != 3)
        total_p += slot_vals[r][c];

      if(r != 0 && r != 1)
        total_a += slot_vals[r][c];
    }

    // Update total row cells with CSS classes
    for(int ti = 0; ti < 2; ti++)
    {
      float total = ti == 0 ? total_p : total_a;
      GtkWidget *tw = widgets->resist_cells[12 + ti][c];

      gtk_widget_remove_css_class(tw, "resist-cell-zero");
      gtk_widget_remove_css_class(tw, "resist-cell-pos");
      gtk_widget_remove_css_class(tw, "resist-cell-high");
      gtk_widget_remove_css_class(tw, "resist-cell-low");

      static const float resist_low[9]  = { 0, 100, 88, 100, 100, 100, 100, 100, 100 };
      static const float resist_high[9] = { 80, 180, 172, 180, 180, 180, 180, 180, 180 };
      char cell[16];

      snprintf(cell, sizeof(cell), "%+d%%", (int)total);
      gtk_label_set_text(GTK_LABEL(tw), cell);
      const char *cls = total >= resist_high[c] ? "resist-cell-high"
                      : total < resist_low[c]   ? "resist-cell-low"
                      : "resist-cell-pos";
      gtk_widget_add_css_class(tw, cls);
    }
  }

  // -- Secondary Resistances --
  static const char *secresist_attrs[8] = {
    "defensiveSlow", "defensiveTrap", "defensiveManaBurnRatio",
    "defensiveDisruption", "defensiveStun", "defensiveFreeze",
    "defensiveSleep", "defensivePetrify"
  };

  float sr_vals[12][8];

  for(int r = 0; r < 12; r++)
  {
    int idx = slot_indices[r];

    for(int c = 0; c < 8; c++)
    {
      float val = 0.0f;

      if(chr->equipment[idx])
        val = item_get_guaranteed_stat(chr->equipment[idx], secresist_attrs[c]);

      sr_vals[r][c] = val;
      GtkWidget *cw = widgets->secresist_cells[r][c];
      gtk_widget_remove_css_class(cw, "resist-cell-pos");
      gtk_widget_remove_css_class(cw, "resist-cell-low");

      if(val > 0.001f)
      {
        char cell[16];

        snprintf(cell, sizeof(cell), "+%d%%", (int)val);
        gtk_label_set_text(GTK_LABEL(cw), cell);
        gtk_widget_add_css_class(cw, "resist-cell-pos");
      }
      else
        gtk_label_set_text(GTK_LABEL(cw), "");
    }
  }

  for(int c = 0; c < 8; c++)
  {
    float total_p = 0.0f, total_a = 0.0f;

    for(int r = 0; r < 12; r++)
    {
      if(r != 2 && r != 3)
        total_p += sr_vals[r][c];

      if(r != 0 && r != 1)
        total_a += sr_vals[r][c];
    }

    for(int ti = 0; ti < 2; ti++)
    {
      float total = ti == 0 ? total_p : total_a;
      GtkWidget *tw = widgets->secresist_cells[12 + ti][c];

      gtk_widget_remove_css_class(tw, "resist-cell-pos");
      gtk_widget_remove_css_class(tw, "resist-cell-low");
      gtk_widget_remove_css_class(tw, "dmg-total-pos");

      if(total > 0.001f)
      {
        char cell[16];

        snprintf(cell, sizeof(cell), "%+d%%", (int)total);
        gtk_label_set_text(GTK_LABEL(tw), cell);
        gtk_widget_add_css_class(tw, "dmg-total-pos");
      }
      else
        gtk_label_set_text(GTK_LABEL(tw), "");
    }
  }

  // -- Direct Damage from item components --
  // Poi = instant poison (offensivePoisonMin), not DOT
  // Vit = reduction to enemy health (offensivePercentCurrentLifeMin), always a percentage
  // Uses mean of (min, max) for guaranteed damage ranges.
  static const struct { const char *min_attr; const char *max_attr; const char *chance; bool pct; } fdmg_defs[8] = {
    { "offensivePhysicalMin",           "offensivePhysicalMax",           "offensivePhysicalChance",           false },
    { "offensivePierceMin",             "offensivePierceMax",             "offensivePierceChance",             false },
    { "offensivePoisonMin",             "offensivePoisonMax",             "offensivePoisonChance",             false },
    { "offensivePercentCurrentLifeMin", "offensivePercentCurrentLifeMax", "offensivePercentCurrentLifeChance", true  },
    { "offensiveElementalMin",          "offensiveElementalMax",          "offensiveElementalChance",          false },
    { "offensiveFireMin",               "offensiveFireMax",               "offensiveFireChance",               false },
    { "offensiveColdMin",               "offensiveColdMax",               "offensiveColdChance",               false },
    { "offensiveLightningMin",          "offensiveLightningMax",          "offensiveLightningChance",          false },
  };

  // Base damage variants (always guaranteed, no chance attr) to add to same columns
  static const struct { const char *min_attr; const char *max_attr; } fdmg_base[8] = {
    { "offensiveBasePhysicalMin",  "offensiveBasePhysicalMax"  },
    { NULL,                        NULL                        },
    { "offensiveBasePoisonMin",    "offensiveBasePoisonMax"    },
    { NULL,                        NULL                        },
    { NULL,                        NULL                        },
    { "offensiveBaseFireMin",      "offensiveBaseFireMax"      },
    { "offensiveBaseColdMin",      "offensiveBaseColdMax"      },
    { "offensiveBaseLightningMin", "offensiveBaseLightningMax" },
  };

  float fd_vals[12][8];

  for(int r = 0; r < 12; r++)
  {
    int idx = slot_indices[r];

    for(int c = 0; c < 8; c++)
    {
      float val = 0.0f;

      if(chr->equipment[idx])
      {
        val = item_get_guaranteed_damage_mean(chr->equipment[idx],
                fdmg_defs[c].min_attr, fdmg_defs[c].max_attr, fdmg_defs[c].chance);

        if(fdmg_base[c].min_attr)
          val += item_get_guaranteed_damage_mean(chr->equipment[idx],
                   fdmg_base[c].min_attr, fdmg_base[c].max_attr, NULL);
      }

      fd_vals[r][c] = val;
      GtkWidget *cw = widgets->fdmg_cells[r][c];
      gtk_widget_remove_css_class(cw, "dmg-total-pos");

      if(val > 0.001f)
      {
        char cell[16];

        if(fdmg_defs[c].pct)
          snprintf(cell, sizeof(cell), "%d%%", (int)val);
        else
          snprintf(cell, sizeof(cell), "%d", (int)val);

        gtk_label_set_text(GTK_LABEL(cw), cell);
      }
      else
        gtk_label_set_text(GTK_LABEL(cw), "");
    }
  }

  for(int c = 0; c < 8; c++)
  {
    float total_p = 0.0f, total_a = 0.0f;

    for(int r = 0; r < 12; r++)
    {
      if(r != 2 && r != 3)
        total_p += fd_vals[r][c];

      if(r != 0 && r != 1)
        total_a += fd_vals[r][c];
    }

    for(int ti = 0; ti < 2; ti++)
    {
      float total = ti == 0 ? total_p : total_a;
      GtkWidget *tw = widgets->fdmg_cells[12 + ti][c];

      gtk_widget_remove_css_class(tw, "dmg-total-pos");

      if(total > 0.001f)
      {
        char cell[16];

        if(fdmg_defs[c].pct)
          snprintf(cell, sizeof(cell), "%d%%", (int)total);
        else
          snprintf(cell, sizeof(cell), "%d", (int)total);

        gtk_label_set_text(GTK_LABEL(tw), cell);
        gtk_widget_add_css_class(tw, "dmg-total-pos");
      }
      else
        gtk_label_set_text(GTK_LABEL(tw), "");
    }
  }

  // -- Bonus Damage -- percentage only from item components --
  static const char *bdmg_pct_attrs[11] = {
    "offensivePhysicalModifier", "offensivePierceModifier",
    "offensiveSlowPoisonModifier", "offensiveSlowBleedingModifier",
    "offensiveLifeModifier", "offensiveElementalModifier",
    "offensiveFireModifier", "offensiveColdModifier", "offensiveLightningModifier",
    "offensiveTotalDamageModifier", "offensiveSlowLifeLeachModifier"
  };

  float bd_pct[12][11];

  for(int r = 0; r < 12; r++)
  {
    int idx = slot_indices[r];

    for(int c = 0; c < 11; c++)
    {
      float pct = 0.0f;

      if(chr->equipment[idx])
        pct = item_get_guaranteed_stat(chr->equipment[idx], bdmg_pct_attrs[c]);

      bd_pct[r][c] = pct;
      GtkWidget *cw = widgets->bdmg_cells[r][c];
      gtk_widget_remove_css_class(cw, "dmg-total-pos");

      if(pct > 0.001f)
      {
        char cell[16];

        snprintf(cell, sizeof(cell), "+%d%%", (int)pct);
        gtk_label_set_text(GTK_LABEL(cw), cell);
      }
      else
        gtk_label_set_text(GTK_LABEL(cw), "");
    }
  }

  for(int c = 0; c < 11; c++)
  {
    float total_p = 0.0f, total_a = 0.0f;

    for(int r = 0; r < 12; r++)
    {
      if(r != 2 && r != 3)
        total_p += bd_pct[r][c];

      if(r != 0 && r != 1)
        total_a += bd_pct[r][c];
    }

    for(int ti = 0; ti < 2; ti++)
    {
      float total = ti == 0 ? total_p : total_a;
      GtkWidget *tw = widgets->bdmg_cells[12 + ti][c];

      gtk_widget_remove_css_class(tw, "dmg-total-pos");

      if(total > 0.001f)
      {
        char cell[16];

        snprintf(cell, sizeof(cell), "+%d%%", (int)total);
        gtk_label_set_text(GTK_LABEL(tw), cell);
        gtk_widget_add_css_class(tw, "dmg-total-pos");
      }
      else
        gtk_label_set_text(GTK_LABEL(tw), "");
    }
  }

  // -- DOT Damage -- flat min*duration from item components --
  static const struct { const char *min; const char *dur; const char *chance; } dot_attrs[8] = {
    { "offensiveSlowFireMin",          "offensiveSlowFireDurationMin",          "offensiveSlowFireChance" },
    { "offensiveSlowColdMin",          "offensiveSlowColdDurationMin",          "offensiveSlowColdChance" },
    { "offensiveSlowLightningMin",     "offensiveSlowLightningDurationMin",     "offensiveSlowLightningChance" },
    { "offensiveSlowPoisonMin",        "offensiveSlowPoisonDurationMin",        "offensiveSlowPoisonChance" },
    { "offensiveSlowBleedingMin",      "offensiveSlowBleedingDurationMin",      "offensiveSlowBleedingChance" },
    { "offensiveSlowLifeMin",          "offensiveSlowLifeDurationMin",          "offensiveSlowLifeChance" },
    { "offensiveSlowManaLeachMin",     "offensiveSlowManaLeachDurationMin",     "offensiveSlowManaLeachChance" },
    { "offensiveSlowLifeLeachMin",     "offensiveSlowLifeLeachDurationMin",     "offensiveSlowLifeLeachChance" },
  };

  float dot_vals[12][8];

  for(int r = 0; r < 12; r++)
  {
    int idx = slot_indices[r];

    for(int c = 0; c < 8; c++)
    {
      float val = 0.0f;

      if(chr->equipment[idx])
        val = item_get_guaranteed_dot(chr->equipment[idx], dot_attrs[c].min, dot_attrs[c].dur, dot_attrs[c].chance);

      dot_vals[r][c] = val;
      GtkWidget *cw = widgets->dotdmg_cells[r][c];
      gtk_widget_remove_css_class(cw, "dmg-total-pos");

      if(val > 0.001f)
      {
        char cell[16];

        snprintf(cell, sizeof(cell), "%d", (int)val);
        gtk_label_set_text(GTK_LABEL(cw), cell);
      }
      else
        gtk_label_set_text(GTK_LABEL(cw), "");
    }
  }

  for(int c = 0; c < 8; c++)
  {
    float total_p = 0.0f, total_a = 0.0f;

    for(int r = 0; r < 12; r++)
    {
      if(r != 2 && r != 3)
        total_p += dot_vals[r][c];

      if(r != 0 && r != 1)
        total_a += dot_vals[r][c];
    }

    for(int ti = 0; ti < 2; ti++)
    {
      float total = ti == 0 ? total_p : total_a;
      GtkWidget *tw = widgets->dotdmg_cells[12 + ti][c];

      gtk_widget_remove_css_class(tw, "dmg-total-pos");

      if(total > 0.001f)
      {
        char cell[16];

        snprintf(cell, sizeof(cell), "%d", (int)total);
        gtk_label_set_text(GTK_LABEL(tw), cell);
        gtk_widget_add_css_class(tw, "dmg-total-pos");
      }
      else
        gtk_label_set_text(GTK_LABEL(tw), "");
    }
  }

  // -- Pet Bonuses -- percentage from petBonusName sub-records --
  // Columns 0-9: damage modifiers, Column 10: pet total speed
  static const char *pet_pct_attrs[11] = {
    "offensivePhysicalModifier", "offensivePierceModifier",
    "offensiveSlowPoisonModifier", "offensiveSlowBleedingModifier",
    "offensiveLifeModifier", "offensiveElementalModifier",
    "offensiveFireModifier", "offensiveColdModifier", "offensiveLightningModifier",
    "offensiveTotalDamageModifier", "characterTotalSpeedModifier"
  };

  float pet_pct[12][11];

  for(int r = 0; r < 12; r++)
  {
    int idx = slot_indices[r];

    for(int c = 0; c < 11; c++)
      pet_pct[r][c] = 0.0f;

    if(chr->equipment[idx])
    {
      TQItem *eq = chr->equipment[idx];
      const char *parts[7] = { eq->base_name, eq->prefix_name, eq->suffix_name,
                               eq->relic_name, eq->relic_bonus, eq->relic_name2, eq->relic_bonus2 };

      for(int p = 0; p < 7; p++)
      {
        if(!parts[p] || !parts[p][0])
          continue;

        TQArzRecordData *data = asset_get_dbr(parts[p]);

        if(!data)
          continue;

        for(uint32_t vi = 0; vi < data->num_vars; vi++)
        {
          if(data->vars[vi].name && strcasecmp(data->vars[vi].name, "petBonusName") == 0)
          {
            TQVariable *v = &data->vars[vi];

            if(v->type == TQ_VAR_STRING && v->count > 0 && v->value.str[0])
            {
              TQArzRecordData *pet = asset_get_dbr(v->value.str[0]);

              if(pet)
              {
                for(uint32_t pi = 0; pi < pet->num_vars; pi++)
                {
                  if(!pet->vars[pi].name)
                    continue;

                  for(int c2 = 0; c2 < 11; c2++)
                  {
                    if(strcasecmp(pet->vars[pi].name, pet_pct_attrs[c2]) == 0)
                    {
                      TQVariable *pv = &pet->vars[pi];

                      if(pv->count > 0 && pv->value.f32)
                        pet_pct[r][c2] += (pv->type == TQ_VAR_INT) ? (float)pv->value.i32[0] : pv->value.f32[0];
                    }
                  }
                }
              }
            }
            break;
          }
        }
      }
    }

    for(int c = 0; c < 11; c++)
    {
      GtkWidget *cw = widgets->petdmg_cells[r][c];

      gtk_widget_remove_css_class(cw, "dmg-total-pos");

      if(pet_pct[r][c] > 0.001f)
      {
        char cell[16];

        snprintf(cell, sizeof(cell), "+%d%%", (int)pet_pct[r][c]);
        gtk_label_set_text(GTK_LABEL(cw), cell);
      }
      else
        gtk_label_set_text(GTK_LABEL(cw), "");
    }
  }

  for(int c = 0; c < 11; c++)
  {
    float total_p = 0.0f, total_a = 0.0f;

    for(int r = 0; r < 12; r++)
    {
      if(r != 2 && r != 3)
        total_p += pet_pct[r][c];

      if(r != 0 && r != 1)
        total_a += pet_pct[r][c];
    }

    for(int ti = 0; ti < 2; ti++)
    {
      float total = ti == 0 ? total_p : total_a;
      GtkWidget *tw = widgets->petdmg_cells[12 + ti][c];

      gtk_widget_remove_css_class(tw, "dmg-total-pos");

      if(total > 0.001f)
      {
        char cell[16];

        snprintf(cell, sizeof(cell), "+%d%%", (int)total);
        gtk_label_set_text(GTK_LABEL(tw), cell);
        gtk_widget_add_css_class(tw, "dmg-total-pos");
      }
      else
        gtk_label_set_text(GTK_LABEL(tw), "");
    }
  }

  // -- Bonus Speed -- percentage from item components --
  static const char *bspd_attrs[6] = {
    "characterAttackSpeedModifier", "characterSpellCastSpeedModifier",
    "characterRunSpeedModifier", "skillProjectileSpeedModifier",
    "skillCooldownReduction", "characterTotalSpeedModifier"
  };

  float bs_pct[12][6];

  for(int r = 0; r < 12; r++)
  {
    int idx = slot_indices[r];

    for(int c = 0; c < 6; c++)
    {
      float pct = 0.0f;

      if(chr->equipment[idx])
        pct = item_get_guaranteed_stat(chr->equipment[idx], bspd_attrs[c]);

      bs_pct[r][c] = pct;
    }

    // Display all 6 columns
    for(int c = 0; c < 6; c++)
    {
      GtkWidget *cw = widgets->bspd_cells[r][c];

      gtk_widget_remove_css_class(cw, "dmg-total-pos");
      float pct = bs_pct[r][c];

      if(c == 4)
      {
        // Recharge: displayed as negative (e.g. -25%)
        if(pct > 0.001f)
        {
          char cell[16];

          snprintf(cell, sizeof(cell), "-%d%%", (int)pct);
          gtk_label_set_text(GTK_LABEL(cw), cell);
        }
        else
          gtk_label_set_text(GTK_LABEL(cw), "");
      }
      else
      {
        if(pct > 0.001f)
        {
          char cell[16];

          snprintf(cell, sizeof(cell), "+%d%%", (int)pct);
          gtk_label_set_text(GTK_LABEL(cw), cell);
        }
        else
          gtk_label_set_text(GTK_LABEL(cw), "");
      }
    }
  }

  for(int c = 0; c < 6; c++)
  {
    float total_p = 0.0f, total_a = 0.0f;

    for(int r = 0; r < 12; r++)
    {
      if(r != 2 && r != 3)
        total_p += bs_pct[r][c];

      if(r != 0 && r != 1)
        total_a += bs_pct[r][c];
    }

    for(int ti = 0; ti < 2; ti++)
    {
      float total = ti == 0 ? total_p : total_a;
      GtkWidget *tw = widgets->bspd_cells[12 + ti][c];

      gtk_widget_remove_css_class(tw, "dmg-total-pos");

      if(total > 0.001f)
      {
        char cell[16];

        if(c == 4)
          snprintf(cell, sizeof(cell), "-%d%%", (int)total);
        else
          snprintf(cell, sizeof(cell), "+%d%%", (int)total);

        gtk_label_set_text(GTK_LABEL(tw), cell);
        gtk_widget_add_css_class(tw, "dmg-total-pos");
      }
      else
        gtk_label_set_text(GTK_LABEL(tw), "");
    }
  }

  // -- Health / Energy Bonuses --
  static const char *hea_attrs[7] = {
    "characterLife", "characterLifeRegen", "characterLifeRegenModifier",
    "characterMana", "characterManaRegen", "characterManaRegenModifier",
    "offensiveLifeLeechMin"
  };
  static const bool hea_is_pct[7] = {
    false, false, true, false, false, true, true
  };

  float hea_vals[12][7];

  for(int r = 0; r < 12; r++)
  {
    int idx = slot_indices[r];

    for(int c = 0; c < 7; c++)
    {
      float val = 0.0f;

      if(chr->equipment[idx])
        val = item_get_guaranteed_stat(chr->equipment[idx], hea_attrs[c]);

      hea_vals[r][c] = val;
      GtkWidget *cw = widgets->hea_cells[r][c];
      gtk_widget_remove_css_class(cw, "dmg-total-pos");

      if(val > 0.001f)
      {
        char cell[16];

        if(hea_is_pct[c])
          snprintf(cell, sizeof(cell), "+%d%%", (int)val);
        else if(c == 1 || c == 4) // regen: float
          snprintf(cell, sizeof(cell), "+%.1f", val);
        else
          snprintf(cell, sizeof(cell), "+%d", (int)val);

        gtk_label_set_text(GTK_LABEL(cw), cell);
      }
      else
        gtk_label_set_text(GTK_LABEL(cw), "");
    }
  }

  for(int c = 0; c < 7; c++)
  {
    float total_p = 0.0f, total_a = 0.0f;

    for(int r = 0; r < 12; r++)
    {
      if(r != 2 && r != 3)
        total_p += hea_vals[r][c];

      if(r != 0 && r != 1)
        total_a += hea_vals[r][c];
    }

    for(int ti = 0; ti < 2; ti++)
    {
      float total = ti == 0 ? total_p : total_a;
      GtkWidget *tw = widgets->hea_cells[12 + ti][c];

      gtk_widget_remove_css_class(tw, "dmg-total-pos");

      if(total > 0.001f)
      {
        char cell[16];

        if(hea_is_pct[c])
          snprintf(cell, sizeof(cell), "+%d%%", (int)total);
        else if(c == 1 || c == 4)
          snprintf(cell, sizeof(cell), "+%.1f", total);
        else
          snprintf(cell, sizeof(cell), "+%d", (int)total);

        gtk_label_set_text(GTK_LABEL(tw), cell);
        gtk_widget_add_css_class(tw, "dmg-total-pos");
      }
      else
        gtk_label_set_text(GTK_LABEL(tw), "");
    }
  }

  // -- Ability Bonuses --
  static const char *abil_attrs[4] = {
    "characterOffensiveAbility", "characterOffensiveAbilityModifier",
    "characterDefensiveAbility", "characterDefensiveAbilityModifier"
  };
  static const bool abil_is_pct[4] = { false, true, false, true };

  float abil_vals[12][4];

  for(int r = 0; r < 12; r++)
  {
    int idx = slot_indices[r];

    for(int c = 0; c < 4; c++)
    {
      float val = 0.0f;

      if(chr->equipment[idx])
        val = item_get_guaranteed_stat(chr->equipment[idx], abil_attrs[c]);

      abil_vals[r][c] = val;
      GtkWidget *cw = widgets->abil_cells[r][c];
      gtk_widget_remove_css_class(cw, "dmg-total-pos");

      if(val > 0.001f)
      {
        char cell[16];

        if(abil_is_pct[c])
          snprintf(cell, sizeof(cell), "+%d%%", (int)val);
        else
          snprintf(cell, sizeof(cell), "+%d", (int)val);

        gtk_label_set_text(GTK_LABEL(cw), cell);
      }
      else
        gtk_label_set_text(GTK_LABEL(cw), "");
    }
  }

  for(int c = 0; c < 4; c++)
  {
    float total_p = 0.0f, total_a = 0.0f;

    for(int r = 0; r < 12; r++)
    {
      if(r != 2 && r != 3)
        total_p += abil_vals[r][c];

      if(r != 0 && r != 1)
        total_a += abil_vals[r][c];
    }

    for(int ti = 0; ti < 2; ti++)
    {
      float total = ti == 0 ? total_p : total_a;
      GtkWidget *tw = widgets->abil_cells[12 + ti][c];

      gtk_widget_remove_css_class(tw, "dmg-total-pos");

      if(total > 0.001f)
      {
        char cell[16];

        if(abil_is_pct[c])
          snprintf(cell, sizeof(cell), "+%d%%", (int)total);
        else
          snprintf(cell, sizeof(cell), "+%d", (int)total);

        gtk_label_set_text(GTK_LABEL(tw), cell);
        gtk_widget_add_css_class(tw, "dmg-total-pos");
      }
      else
        gtk_label_set_text(GTK_LABEL(tw), "");
    }
  }
}

// Update the main character info panel and trigger stat table refresh.
// @param widgets  application widget tree
// @param chr      newly-loaded character (takes ownership if different from current)
void
update_ui(AppWidgets *widgets, TQCharacter *chr)
{
  char buffer[256];

  if(widgets->current_character && widgets->current_character != chr)
    character_free(widgets->current_character);

  widgets->current_character = chr;
  widgets->char_dirty = false;
  update_save_button_sensitivity(widgets);

  if(widgets->checklist_btn)
    gtk_widget_set_sensitive(widgets->checklist_btn, chr != NULL);

  if(widgets->stats_btn)
    gtk_widget_set_sensitive(widgets->stats_btn, chr != NULL);

  if(widgets->skills_btn)
    gtk_widget_set_sensitive(widgets->skills_btn, chr != NULL);

  gtk_label_set_text(GTK_LABEL(widgets->name_label), chr->character_name);

  snprintf(buffer, sizeof(buffer), "%u", chr->level);
  gtk_label_set_text(GTK_LABEL(widgets->level_label), buffer);

  gtk_label_set_text(GTK_LABEL(widgets->mastery1_label),
                     chr->mastery1 ? mastery_display_name(chr->mastery1) : "-");

  gtk_label_set_text(GTK_LABEL(widgets->mastery2_label),
                     chr->mastery2 ? mastery_display_name(chr->mastery2) : "-");

  snprintf(buffer, sizeof(buffer), "%.0f", chr->strength);
  gtk_label_set_text(GTK_LABEL(widgets->strength_label), buffer);

  snprintf(buffer, sizeof(buffer), "%.0f", chr->dexterity);
  gtk_label_set_text(GTK_LABEL(widgets->dexterity_label), buffer);

  snprintf(buffer, sizeof(buffer), "%.0f", chr->intelligence);
  gtk_label_set_text(GTK_LABEL(widgets->intelligence_label), buffer);

  snprintf(buffer, sizeof(buffer), "%.0f", chr->health);
  gtk_label_set_text(GTK_LABEL(widgets->health_label), buffer);

  snprintf(buffer, sizeof(buffer), "%.0f", chr->mana);
  gtk_label_set_text(GTK_LABEL(widgets->mana_label), buffer);

  snprintf(buffer, sizeof(buffer), "%u", chr->deaths);
  gtk_label_set_text(GTK_LABEL(widgets->deaths_label), buffer);

  snprintf(buffer, sizeof(buffer), "%u", chr->kills);
  gtk_label_set_text(GTK_LABEL(widgets->kills_label), buffer);

  // Total armor: sum defensiveProtection across all equipped items, applying
  // any item-local defensiveProtectionModifier percentage bonus.
  float total_armor = 0.0f;

  for(int i = 0; i < 12; i++)
  {
    if(!chr->equipment[i])
      continue;

    float base = item_get_guaranteed_stat(chr->equipment[i], "defensiveProtection");

    if(base <= 0.001f)
      continue;

    float pct = item_get_guaranteed_stat(chr->equipment[i], "defensiveProtectionModifier");

    total_armor += base * (1.0f + pct / 100.0f);
  }

  snprintf(buffer, sizeof(buffer), "%d", (int)(total_armor + 0.5f));
  gtk_label_set_text(GTK_LABEL(widgets->armor_label), buffer);

  gtk_widget_queue_draw(widgets->equip_drawing_area);
  gtk_widget_queue_draw(widgets->inv_drawing_area);
  gtk_widget_queue_draw(widgets->bag_drawing_area);

  update_resist_damage_tables(widgets, chr);
}

// Build all stat table grid widgets (resistances, damage, speed, health, abilities)
// and attach them to the tables_inner container.
// @param widgets       application widget tree whose cell arrays get populated
// @param tables_inner  vertical box container to append table sections into
void
build_stat_tables(AppWidgets *widgets, GtkWidget *tables_inner)
{
  // Row labels shared by all tables
  static const char *row_labels[14] = {
    "Right", "Left", "AltRight", "AltLeft",
    "Ring 1", "Ring 2", "Neck", "Head",
    "Torso", "Legs", "Arms", "Artifact",
    "Tot (P)", "Tot (A)"
  };

  // -- Helper macro: build a table section --
  #define BUILD_TABLE_SECTION(vbox_var, title_str, grid_widget, ncols, hdrs, hdr_css, cells_array) \
  do { \
    GtkWidget *vbox_var = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2); \
    gtk_widget_set_hexpand(vbox_var, TRUE); \
    gtk_widget_set_vexpand(vbox_var, FALSE); \
    gtk_widget_add_css_class(vbox_var, "resist-frame"); \
    gtk_box_append(GTK_BOX(tables_inner), vbox_var); \
    GtkWidget *hdr_lbl = gtk_label_new(title_str); \
    gtk_widget_set_halign(hdr_lbl, GTK_ALIGN_START); \
    gtk_widget_add_css_class(hdr_lbl, "resist-title"); \
    gtk_box_append(GTK_BOX(vbox_var), hdr_lbl); \
    grid_widget = gtk_grid_new(); \
    gtk_grid_set_column_spacing(GTK_GRID(grid_widget), 2); \
    gtk_grid_set_row_spacing(GTK_GRID(grid_widget), 0); \
    gtk_widget_add_css_class(grid_widget, "resist-grid"); \
    gtk_box_append(GTK_BOX(vbox_var), grid_widget); \
    for(int _c = 0; _c < ncols; _c++) \
    { \
      GtkWidget *_cl = gtk_label_new(hdrs[_c]); \
      gtk_widget_add_css_class(_cl, "resist-col-hdr"); \
      gtk_widget_add_css_class(_cl, hdr_css[_c]); \
      gtk_grid_attach(GTK_GRID(grid_widget), _cl, _c + 1, 0, 1, 1); \
    } \
    for(int _r = 0; _r < 14; _r++) \
    { \
      GtkWidget *_rl = gtk_label_new(row_labels[_r]); \
      gtk_widget_set_halign(_rl, GTK_ALIGN_START); \
      gtk_widget_add_css_class(_rl, _r < 12 ? "resist-row-hdr" : "resist-total-label"); \
      if(_r < 12 && (_r & 1)) \
        gtk_widget_add_css_class(_rl, "resist-row-alt"); \
      gtk_grid_attach(GTK_GRID(grid_widget), _rl, 0, _r + 1, 1, 1); \
    } \
    for(int _r = 0; _r < 14; _r++) \
    { \
      for(int _c = 0; _c < ncols; _c++) \
      { \
        GtkWidget *_dl = gtk_label_new(""); \
        gtk_widget_set_halign(_dl, GTK_ALIGN_END); \
        gtk_widget_add_css_class(_dl, _r < 12 ? "dmg-cell" : "resist-total"); \
        if(_r < 12 && (_r & 1)) \
          gtk_widget_add_css_class(_dl, "resist-row-alt"); \
        gtk_grid_attach(GTK_GRID(grid_widget), _dl, _c + 1, _r + 1, 1, 1); \
        cells_array[_r][_c] = _dl; \
      } \
    } \
  } while(0)

  // -- 1. Resistances table (uses resist-cell styling, not dmg-cell) --
  {
    GtkWidget *resist_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_hexpand(resist_vbox, TRUE);
    gtk_widget_set_vexpand(resist_vbox, FALSE);
    gtk_widget_add_css_class(resist_vbox, "resist-frame");
    gtk_box_append(GTK_BOX(tables_inner), resist_vbox);

    GtkWidget *resist_header = gtk_label_new("RESISTANCES");

    gtk_widget_set_halign(resist_header, GTK_ALIGN_START);
    gtk_widget_add_css_class(resist_header, "resist-title");
    gtk_box_append(GTK_BOX(resist_vbox), resist_header);

    widgets->resist_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(widgets->resist_grid), 2);
    gtk_grid_set_row_spacing(GTK_GRID(widgets->resist_grid), 0);
    gtk_widget_add_css_class(widgets->resist_grid, "resist-grid");
    gtk_box_append(GTK_BOX(resist_vbox), widgets->resist_grid);

    static const char *col_headers[9] = { "Phy", "Prc", "Poi", "Ble", "Vit", "Ele", "Fir", "Cld", "Ltn" };
    static const char *col_css[9] = {
      "resist-hdr-phy", "resist-hdr-prc", "resist-hdr-psn", "resist-hdr-ble",
      "resist-hdr-vit", "resist-hdr-ele", "resist-hdr-fir", "resist-hdr-cld", "resist-hdr-ltn"
    };

    for(int c = 0; c < 9; c++)
    {
      GtkWidget *lbl = gtk_label_new(col_headers[c]);

      gtk_widget_add_css_class(lbl, "resist-col-hdr");
      gtk_widget_add_css_class(lbl, col_css[c]);
      gtk_grid_attach(GTK_GRID(widgets->resist_grid), lbl, c + 1, 0, 1, 1);
    }

    for(int r = 0; r < 14; r++)
    {
      GtkWidget *lbl = gtk_label_new(row_labels[r]);

      gtk_widget_set_halign(lbl, GTK_ALIGN_START);
      gtk_widget_add_css_class(lbl, r < 12 ? "resist-row-hdr" : "resist-total-label");

      if(r < 12 && (r & 1))
        gtk_widget_add_css_class(lbl, "resist-row-alt");

      gtk_grid_attach(GTK_GRID(widgets->resist_grid), lbl, 0, r + 1, 1, 1);
    }

    for(int r = 0; r < 14; r++)
    {
      for(int c = 0; c < 9; c++)
      {
        GtkWidget *lbl = gtk_label_new("\xe2\x80\x94");

        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_widget_add_css_class(lbl, r < 12 ? "resist-cell" : "resist-total");
        gtk_widget_add_css_class(lbl, "resist-cell-zero");

        if(r < 12 && (r & 1))
          gtk_widget_add_css_class(lbl, "resist-row-alt");

        gtk_grid_attach(GTK_GRID(widgets->resist_grid), lbl, c + 1, r + 1, 1, 1);
        widgets->resist_cells[r][c] = lbl;
      }
    }
  }

  // -- 2. Secondary Resistances --
  {
    static const char *sr_hdrs[8] = { "Slow", "Trap", "Energy", "Disrupt", "Stun", "Freeze", "Sleep", "Petrify" };
    static const char *sr_css[8] = {
      "secresist-hdr-slo", "secresist-hdr-trp", "secresist-hdr-ene", "secresist-hdr-dis",
      "secresist-hdr-stn", "secresist-hdr-frz", "secresist-hdr-sle", "secresist-hdr-pet"
    };

    BUILD_TABLE_SECTION(sr_vbox, "SECONDARY RESISTANCES", widgets->secresist_grid, 8, sr_hdrs, sr_css, widgets->secresist_cells);
  }

  // -- 2b. Flat Damage --
  {
    static const char *fd_hdrs[8] = { "Phy", "Prc", "Poi", "Vit", "Ele", "Fir", "Cld", "Ltn" };
    static const char *fd_css[8] = {
      "dmg-hdr-phy", "dmg-hdr-prc", "dmg-hdr-psn",
      "dmg-hdr-vit", "dmg-hdr-ele", "dmg-hdr-fir", "dmg-hdr-cld", "dmg-hdr-ltn"
    };

    BUILD_TABLE_SECTION(fd_vbox, "DIRECT DAMAGE", widgets->fdmg_grid, 8, fd_hdrs, fd_css, widgets->fdmg_cells);
  }

  // -- 3. Bonus Damage --
  {
    static const char *bd_hdrs[11] = { "Phy", "Prc", "Poi", "Ble", "Vit", "Ele", "Fir", "Cld", "Ltn", "Tot", "LL" };
    static const char *bd_css[11] = {
      "dmg-hdr-phy", "dmg-hdr-prc", "dmg-hdr-psn", "dmg-hdr-ble",
      "dmg-hdr-vit", "dmg-hdr-ele", "dmg-hdr-fir", "dmg-hdr-cld", "dmg-hdr-ltn", "dmg-hdr-tot", "dmg-hdr-ll"
    };

    BUILD_TABLE_SECTION(bd_vbox, "BONUS DAMAGE", widgets->bdmg_grid, 11, bd_hdrs, bd_css, widgets->bdmg_cells);
  }

  // -- 3b. DOT Damage --
  {
    static const char *dot_hdrs[8] = { "Burn", "FBurn", "EBurn", "Poi", "Ble", "Vit", "EnBurn", "LL" };
    static const char *dot_css[8] = {
      "dot-hdr-brn", "dot-hdr-fbrn", "dot-hdr-ebrn", "dot-hdr-psn",
      "dot-hdr-ble", "dot-hdr-vit", "dot-hdr-el", "dot-hdr-ll"
    };

    BUILD_TABLE_SECTION(dot_vbox, "DOT DAMAGE", widgets->dotdmg_grid, 8, dot_hdrs, dot_css, widgets->dotdmg_cells);
  }

  // -- 4. Pet Bonuses --
  {
    static const char *pb_hdrs[11] = { "Phy", "Prc", "Poi", "Ble", "Vit", "Ele", "Fir", "Cld", "Ltn", "Tot", "Spd" };
    static const char *pb_css[11] = {
      "pet-hdr-phy", "pet-hdr-prc", "pet-hdr-psn", "pet-hdr-ble",
      "pet-hdr-vit", "pet-hdr-ele", "pet-hdr-fir", "pet-hdr-cld", "pet-hdr-ltn", "pet-hdr-tot", "pet-hdr-spd"
    };

    BUILD_TABLE_SECTION(pb_vbox, "PET BONUSES", widgets->petdmg_grid, 11, pb_hdrs, pb_css, widgets->petdmg_cells);
  }

  // -- 5. Bonus Speed --
  {
    static const char *bs_hdrs[6] = { "Attack", "Casting", "Movement", "Projtile", "Recharge", "Total" };
    static const char *bs_css[6] = {
      "spd-hdr-atk", "spd-hdr-cast", "spd-hdr-move", "spd-hdr-proj",
      "spd-hdr-rech", "spd-hdr-tot"
    };

    BUILD_TABLE_SECTION(bs_vbox, "BONUS SPEED", widgets->bspd_grid, 6, bs_hdrs, bs_css, widgets->bspd_cells);
  }

  // -- 6. Health / Energy Bonuses --
  {
    static const char *hea_hdrs[7] = { "HP", "HReg", "HR%", "EP", "EReg", "ER%", "ADCTH" };
    static const char *hea_css[7] = {
      "hea-hdr-hp", "hea-hdr-hreg", "hea-hdr-hrpct", "hea-hdr-ep", "hea-hdr-ereg",
      "hea-hdr-erpct", "hea-hdr-adcth"
    };

    BUILD_TABLE_SECTION(hea_vbox, "HEALTH / ENERGY BONUSES", widgets->hea_grid, 7, hea_hdrs, hea_css, widgets->hea_cells);
  }

  // -- 7. Ability Bonuses --
  {
    static const char *abil_hdrs[4] = { "OA", "OA%", "DA", "DA%" };
    static const char *abil_css[4] = {
      "abil-hdr-oa", "abil-hdr-oapct", "abil-hdr-da", "abil-hdr-dapct"
    };

    BUILD_TABLE_SECTION(abil_vbox, "ABILITY BONUSES", widgets->abil_grid, 4, abil_hdrs, abil_css, widgets->abil_cells);
  }

  #undef BUILD_TABLE_SECTION
}
