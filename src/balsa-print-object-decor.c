/* -*-mode:c; c-style:k&r; c-basic-offset:4; -*- */
/* Balsa E-Mail Client
 * Copyright (C) 1997-2016 Stuart Parmenter and others
 * Written by (C) Albrecht Dreﬂ <albrecht.dress@arcor.de> 2007
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#if defined(HAVE_CONFIG_H) && HAVE_CONFIG_H
#   include "config.h"
#endif                          /* HAVE_CONFIG_H */
#include "balsa-print-object-decor.h"

#include <gtk/gtk.h>


/* object related functions */
static void balsa_print_object_decor_finalize(GObject *self);

static void balsa_print_object_decor_draw(BalsaPrintObject *self,
                                          GtkPrintContext  *context,
                                          cairo_t          *cairo_ctx);


typedef enum {
    BALSA_PRINT_DECOR_FRAME_BEGIN,
    BALSA_PRINT_DECOR_FRAME_END,
    BALSA_PRINT_DECOR_SEPARATOR
} BalsaPrintDecorType;

struct _BalsaPrintObjectDecor {
    BalsaPrintObject parent;

    BalsaPrintDecorType mode;
    gchar *label;
};

G_DEFINE_TYPE(BalsaPrintObjectDecor,
              balsa_print_object_decor,
              BALSA_TYPE_PRINT_OBJECT)


static void
balsa_print_object_decor_class_init(BalsaPrintObjectDecorClass *klass)
{
    BALSA_PRINT_OBJECT_CLASS(klass)->draw = balsa_print_object_decor_draw;
    G_OBJECT_CLASS(klass)->finalize       = balsa_print_object_decor_finalize;
}


static void
balsa_print_object_decor_init(BalsaPrintObjectDecor *pod)
{
    pod->label = NULL;
}


static void
balsa_print_object_decor_finalize(GObject *self)
{
    BalsaPrintObjectDecor *pod = BALSA_PRINT_OBJECT_DECOR(self);

    g_free(pod->label);

    G_OBJECT_CLASS(balsa_print_object_decor_parent_class)->finalize(self);
}


static BalsaPrintObject *
decor_new_real(BalsaPrintSetup    *psetup,
               BalsaPrintDecorType mode)
{
    BalsaPrintObjectDecor *pod;
    BalsaPrintObject *po;

    pod = g_object_new(BALSA_TYPE_PRINT_OBJECT_DECOR, NULL);
    g_assert(pod != NULL);
    po = BALSA_PRINT_OBJECT(pod);

    /* create the part */
    balsa_print_object_set_depth(po, psetup->curr_depth);
    pod->mode = mode;

    /* check if it should start on a new page */
    if (psetup->c_y_pos + C_SEPARATOR > psetup->c_height) {
        psetup->page_count++;
        psetup->c_y_pos = 0;
    }

    /* remember the extent */
    balsa_print_object_set_on_page(po, psetup->page_count - 1);
    balsa_print_object_set_c_at_x(po, psetup->c_x0 + balsa_print_object_get_depth(
                                      po) * C_LABEL_SEP);
    balsa_print_object_set_c_at_y(po, psetup->c_y0 + psetup->c_y_pos);
    balsa_print_object_set_c_width(po, psetup->c_width - 2 * balsa_print_object_get_depth(
                                       po) * C_LABEL_SEP);
    balsa_print_object_set_c_height(po, C_SEPARATOR);

    /* adjust the y position */
    psetup->c_y_pos += C_SEPARATOR;

    return po;
}


GList *
balsa_print_object_separator(GList           *list,
                             BalsaPrintSetup *psetup)
{
    return g_list_append(list, decor_new_real(psetup, BALSA_PRINT_DECOR_SEPARATOR));
}


GList *
balsa_print_object_frame_begin(GList           *list,
                               const gchar     *label,
                               BalsaPrintSetup *psetup)
{
    BalsaPrintObjectDecor *pod;
    BalsaPrintObject *po;

    pod = g_object_new(BALSA_TYPE_PRINT_OBJECT_DECOR, NULL);
    g_assert(pod != NULL);
    po = BALSA_PRINT_OBJECT(pod);

    /* create the part */
    balsa_print_object_set_depth(po, psetup->curr_depth++);
    pod->mode = BALSA_PRINT_DECOR_FRAME_BEGIN;
    if (label) {
        pod->label = g_strdup(label);
        balsa_print_object_set_c_height(po,
                                        MAX(P_TO_C(psetup->p_hdr_font_height),
                                            C_SEPARATOR)
                                        + C_SEPARATOR * 0.5);
    } else {
        balsa_print_object_set_c_height(po, C_SEPARATOR);
    }

    /* check if it should start on a new page */
    if (psetup->c_y_pos + balsa_print_object_get_c_height(po) +
        3 * P_TO_C(psetup->p_body_font_height) > psetup->c_height) {
        psetup->page_count++;
        psetup->c_y_pos = 0;
    }

    /* remember the extent */
    balsa_print_object_set_on_page(po, psetup->page_count - 1);
    balsa_print_object_set_c_at_x(po, psetup->c_x0 + balsa_print_object_get_depth(
                                      po) * C_LABEL_SEP);
    balsa_print_object_set_c_at_y(po, psetup->c_y0 + psetup->c_y_pos);
    balsa_print_object_set_c_width(po, psetup->c_width - 2 * balsa_print_object_get_depth(
                                       po) * C_LABEL_SEP);

    /* adjust the y position */
    psetup->c_y_pos += balsa_print_object_get_c_height(po);

    return g_list_append(list, po);
}


GList *
balsa_print_object_frame_end(GList           *list,
                             BalsaPrintSetup *psetup)
{
    g_return_val_if_fail(psetup->curr_depth > 0, list);
    psetup->curr_depth--;
    return g_list_append(list, decor_new_real(psetup, BALSA_PRINT_DECOR_FRAME_END));
}


static void
balsa_print_object_decor_draw(BalsaPrintObject *self,
                              GtkPrintContext  *context,
                              cairo_t          *cairo_ctx)
{
    BalsaPrintObjectDecor *pod;

    pod = BALSA_PRINT_OBJECT_DECOR(self);
    g_assert(pod != NULL);

    /* draw the decor */
    cairo_save(cairo_ctx);
    cairo_new_path(cairo_ctx);

    switch (pod->mode) {
    case BALSA_PRINT_DECOR_SEPARATOR:
        cairo_set_line_width(cairo_ctx, 1.0);
        cairo_move_to(cairo_ctx, balsa_print_object_get_c_at_x(
                          self), balsa_print_object_get_c_at_y(self) + 0.5 * C_SEPARATOR);
        cairo_line_to(cairo_ctx, balsa_print_object_get_c_at_x(
                          self) + balsa_print_object_get_c_width(self),
                      balsa_print_object_get_c_at_y(self) + 0.5 * C_SEPARATOR);
        break;

    case BALSA_PRINT_DECOR_FRAME_END:
        cairo_set_line_width(cairo_ctx, 0.25);
        cairo_move_to(cairo_ctx, balsa_print_object_get_c_at_x(
                          self), balsa_print_object_get_c_at_y(self));
        cairo_line_to(cairo_ctx, balsa_print_object_get_c_at_x(
                          self), balsa_print_object_get_c_at_y(self) + 0.5 * C_SEPARATOR);
        cairo_line_to(cairo_ctx, balsa_print_object_get_c_at_x(
                          self) + balsa_print_object_get_c_width(self),
                      balsa_print_object_get_c_at_y(self) + 0.5 * C_SEPARATOR);
        cairo_line_to(cairo_ctx, balsa_print_object_get_c_at_x(
                          self) + balsa_print_object_get_c_width(
                          self), balsa_print_object_get_c_at_y(self));
        break;

    case BALSA_PRINT_DECOR_FRAME_BEGIN:
        cairo_set_line_width(cairo_ctx, 0.25);
        cairo_move_to(cairo_ctx, balsa_print_object_get_c_at_x(
                          self), balsa_print_object_get_c_at_y(
                          self) + balsa_print_object_get_c_height(self));
        if (pod->label) {
            PangoLayout *layout;
            PangoFontDescription *font;
            gint p_label_width;
            gint p_label_height;
            gdouble c_y_hor_line;

            /* print the label */
            font   = pango_font_description_from_string(balsa_app.print_header_font);
            layout = gtk_print_context_create_pango_layout(context);
            pango_layout_set_font_description(layout, font);
            pango_font_description_free(font);
            pango_layout_set_text(layout, pod->label, -1);
            pango_layout_get_size(layout, &p_label_width, &p_label_height);
            c_y_hor_line = balsa_print_object_get_c_at_y(self) +
                balsa_print_object_get_c_height(self) -
                MAX(P_TO_C(p_label_height), C_SEPARATOR) * 0.5;
            cairo_line_to(cairo_ctx, balsa_print_object_get_c_at_x(self), c_y_hor_line);
            cairo_line_to(cairo_ctx, balsa_print_object_get_c_at_x(
                              self) + C_LABEL_SEP, c_y_hor_line);
            cairo_move_to(cairo_ctx, balsa_print_object_get_c_at_x(self) + 1.5 * C_LABEL_SEP,
                          c_y_hor_line - 0.5 * P_TO_C(p_label_height));
            pango_cairo_show_layout(cairo_ctx, layout);
            g_object_unref(layout);
            cairo_move_to(cairo_ctx, balsa_print_object_get_c_at_x(
                              self) + 2.0 * C_LABEL_SEP + P_TO_C(p_label_width),
                          c_y_hor_line);
            cairo_line_to(cairo_ctx, balsa_print_object_get_c_at_x(
                              self) + balsa_print_object_get_c_width(self), c_y_hor_line);
        } else {
            cairo_line_to(cairo_ctx, balsa_print_object_get_c_at_x(self),
                          balsa_print_object_get_c_at_y(self) +
                          balsa_print_object_get_c_height(self) - 0.5 * C_SEPARATOR);
            cairo_line_to(cairo_ctx, balsa_print_object_get_c_at_x(
                              self) + balsa_print_object_get_c_width(self),
                          balsa_print_object_get_c_at_y(self) +
                          balsa_print_object_get_c_height(self) - 0.5 * C_SEPARATOR);
        }
        cairo_line_to(cairo_ctx, balsa_print_object_get_c_at_x(
                          self) + balsa_print_object_get_c_width(self),
                      balsa_print_object_get_c_at_y(self) + balsa_print_object_get_c_height(
                          self));
        break;
    }

    cairo_stroke(cairo_ctx);
    cairo_restore(cairo_ctx);
}
