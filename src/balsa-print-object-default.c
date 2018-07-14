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
#include "balsa-print-object-default.h"

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include "balsa-icons.h"
#include "balsa-print-object.h"
#include "balsa-print-object-text.h"
#include "libbalsa-vfs.h"
#include "rfc2445.h"


/* object related functions */
static void balsa_print_object_default_dispose(GObject *self);
static void balsa_print_object_default_finalize(GObject *self);

static void balsa_print_object_default_draw(BalsaPrintObject *self,
                                            GtkPrintContext  *context,
                                            cairo_t          *cairo_ctx);


struct _BalsaPrintObjectDefault {
    BalsaPrintObject parent;

    gint p_label_width;
    gdouble c_image_width;
    gdouble c_image_height;
    gdouble c_text_height;
    gchar *description;
    GdkPixbuf *pixbuf;
};


G_DEFINE_TYPE(BalsaPrintObjectDefault,
              balsa_print_object_default,
              BALSA_TYPE_PRINT_OBJECT)


static void
balsa_print_object_default_class_init(BalsaPrintObjectDefaultClass *klass)
{
    BALSA_PRINT_OBJECT_CLASS(klass)->draw = balsa_print_object_default_draw;
    G_OBJECT_CLASS(klass)->dispose        = balsa_print_object_default_dispose;
    G_OBJECT_CLASS(klass)->finalize       = balsa_print_object_default_finalize;
}


static void
balsa_print_object_default_init(BalsaPrintObjectDefault *pod)
{
    pod->pixbuf      = NULL;
    pod->description = NULL;
}


static void
balsa_print_object_default_dispose(GObject *self)
{
    BalsaPrintObjectDefault *pod = BALSA_PRINT_OBJECT_DEFAULT(self);

    g_clear_object(&pod->pixbuf);

    G_OBJECT_CLASS(balsa_print_object_default_parent_class)->dispose(self);
}


static void
balsa_print_object_default_finalize(GObject *self)
{
    BalsaPrintObjectDefault *pod = BALSA_PRINT_OBJECT_DEFAULT(self);

    g_free(pod->description);

    G_OBJECT_CLASS(balsa_print_object_default_parent_class)->finalize(self);
}


GList *
balsa_print_object_default(GList               *list,
                           GtkPrintContext     *context,
                           LibBalsaMessageBody *body,
                           BalsaPrintSetup     *psetup)
{
    BalsaPrintObjectDefault *pod;
    BalsaPrintObject *po;
    gchar *conttype;
    PangoFontDescription *header_font;
    PangoLayout *test_layout;
    PangoTabArray *tabs;
    GString *desc_buf;
    gdouble c_max_height;
    gchar *part_desc;

    pod = g_object_new(BALSA_TYPE_PRINT_OBJECT_DEFAULT, NULL);
    g_assert(pod != NULL);
    po = BALSA_PRINT_OBJECT(pod);

    /* create the part */
    balsa_print_object_set_depth(po, psetup->curr_depth);
    balsa_print_object_set_c_width(po,
                                   psetup->c_width
                                   - 2 * psetup->curr_depth * C_LABEL_SEP);

    /* get a pixbuf according to the mime type */
    conttype            = libbalsa_message_body_get_mime_type(body);
    pod->pixbuf         = libbalsa_icon_finder(NULL, conttype, NULL, NULL, 48);
    pod->c_image_width  = gdk_pixbuf_get_width(pod->pixbuf);
    pod->c_image_height = gdk_pixbuf_get_height(pod->pixbuf);


    /* create a layout for calculating the maximum label width */
    header_font =
        pango_font_description_from_string(balsa_app.print_header_font);
    test_layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_font_description(test_layout, header_font);
    pango_font_description_free(header_font);
    desc_buf = g_string_new("");

    /* add type and filename (if available) */
    pod->p_label_width =
        p_string_width_from_layout(test_layout, _("Type:"));
    if ((part_desc = libbalsa_vfs_content_description(conttype)))
        g_string_append_printf(desc_buf, "%s\t%s (%s)", _("Type:"),
                               part_desc, conttype);
    else
        g_string_append_printf(desc_buf, "%s\t%s", _("Type:"), conttype);
    g_free(part_desc);
    g_free(conttype);
    if (body->filename) {
        gint p_fnwidth =
            p_string_width_from_layout(test_layout, _("File name:"));

        if (p_fnwidth > pod->p_label_width)
            pod->p_label_width = p_fnwidth;
        g_string_append_printf(desc_buf, "\n%s\t%s", _("File name:"),
                               body->filename);
    }

    /* add a small space between label and value */
    pod->p_label_width += C_TO_P(C_LABEL_SEP);

    /* configure the layout so we can calculate the text height */
    pango_layout_set_indent(test_layout, -pod->p_label_width);
    tabs =
        pango_tab_array_new_with_positions(1, FALSE, PANGO_TAB_LEFT,
                                           pod->p_label_width);
    pango_layout_set_tabs(test_layout, tabs);
    pango_tab_array_free(tabs);
    pango_layout_set_width(test_layout,
                           C_TO_P(balsa_print_object_get_c_width(po) -
                                  4 * C_LABEL_SEP - pod->c_image_width));
    pango_layout_set_alignment(test_layout, PANGO_ALIGN_LEFT);
    pod->c_text_height =
        P_TO_C(p_string_height_from_layout(test_layout, desc_buf->str));
    pod->description = g_string_free(desc_buf, FALSE);

    /* check if we should move to the next page */
    c_max_height = MAX(pod->c_text_height, pod->c_image_height);
    if (psetup->c_y_pos + c_max_height > psetup->c_height) {
        psetup->c_y_pos = 0;
        psetup->page_count++;
    }

    /* remember the extent */
    balsa_print_object_set_on_page(po, psetup->page_count - 1);
    balsa_print_object_set_c_at_x(po, psetup->c_x0 + balsa_print_object_get_depth(
                                      po) * C_LABEL_SEP);
    balsa_print_object_set_c_at_y(po, psetup->c_y0 + psetup->c_y_pos);
    balsa_print_object_set_c_width(po, psetup->c_width - 2 * balsa_print_object_get_depth(
                                       po) * C_LABEL_SEP);
    balsa_print_object_set_c_height(po, c_max_height);

    /* adjust the y position */
    psetup->c_y_pos += c_max_height;

    return g_list_append(list, po);
}


static void
balsa_print_object_default_draw(BalsaPrintObject *self,
                                GtkPrintContext  *context,
                                cairo_t          *cairo_ctx)
{
    BalsaPrintObjectDefault *pod;
    gdouble c_max_height;
    gdouble c_offset;
    PangoLayout *layout;
    PangoFontDescription *font;
    PangoTabArray *tabs;

    /* set up */
    pod = BALSA_PRINT_OBJECT_DEFAULT(self);
    g_assert(pod != NULL);
    c_max_height = MAX(pod->c_text_height, pod->c_image_height);
    c_offset     = pod->c_image_width + 4 * C_LABEL_SEP;

    /* print the icon */
    if (pod->pixbuf)
        cairo_print_pixbuf(cairo_ctx, pod->pixbuf, balsa_print_object_get_c_at_x(self),
                           balsa_print_object_get_c_at_y(self), 1.0);

    /* print the description */
    font   = pango_font_description_from_string(balsa_app.print_header_font);
    layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_indent(layout, -pod->p_label_width);
    tabs =
        pango_tab_array_new_with_positions(1, FALSE, PANGO_TAB_LEFT,
                                           pod->p_label_width);
    pango_layout_set_tabs(layout, tabs);
    pango_tab_array_free(tabs);
    pango_layout_set_width(layout, C_TO_P(balsa_print_object_get_c_width(self) - c_offset));
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_text(layout, pod->description, -1);
    cairo_move_to(cairo_ctx, balsa_print_object_get_c_at_x(self) + c_offset,
                  balsa_print_object_get_c_at_y(self) + (c_max_height -
                                                         pod->c_text_height) * 0.5);
    pango_cairo_show_layout(cairo_ctx, layout);
    g_object_unref(layout);
}


/*
 * Following code was moved from balsa-print-object-text, because it
 * extensively uses balsa-print-object-default,
 * and calls only balsa_print_object_text() from
 * balsa-print-object-text.
 */

/* note: a vcard is an icon plus a series of labels/text, so this function actually
 * returns a BalsaPrintObjectDefault... */

#define ADD_VCARD_FIELD(buf, labwidth, layout, field, descr)            \
    do {                                                                \
        if (field) {                                                    \
            gint label_width = p_string_width_from_layout(layout, descr); \
            if (label_width > labwidth)                                 \
                labwidth = label_width;                                 \
            if ((buf)->len > 0)                                         \
                g_string_append_c(buf, '\n');                           \
            g_string_append_printf(buf, "%s\t%s", descr, field);        \
        }                                                               \
    } while (0)

GList *
balsa_print_object_vcard(GList               *list,
                         GtkPrintContext     *context,
                         LibBalsaMessageBody *body,
                         BalsaPrintSetup     *psetup)
{
    BalsaPrintObjectDefault *pod;
    BalsaPrintObject *po;
    PangoFontDescription *header_font;
    PangoLayout *test_layout;
    PangoTabArray *tabs;
    GString *desc_buf;
    gdouble c_max_height;
    LibBalsaAddress *address = NULL;
    gchar *textbuf;

    /* check if we can create an address from the body and fall back to default if
     * this fails */
    if (body->buffer)
        textbuf = g_strdup(body->buffer);
    else
        libbalsa_message_body_get_content(body, &textbuf, NULL);
    if (textbuf)
        address = libbalsa_address_new_from_vcard(textbuf, body->charset);
    if (address == NULL) {
        g_free(textbuf);
        return balsa_print_object_text(list, context, body, psetup);
    }

    /* proceed with the address information */
    pod = g_object_new(BALSA_TYPE_PRINT_OBJECT_DEFAULT, NULL);
    g_assert(pod != NULL);
    po = BALSA_PRINT_OBJECT(pod);

    /* create the part */
    balsa_print_object_set_depth(po, psetup->curr_depth);
    balsa_print_object_set_c_width(po,
                                   psetup->c_width
                                   - 2 * psetup->curr_depth * C_LABEL_SEP);

    /* get the stock contacts icon or the mime type icon on fail */
    pod->pixbuf =
        gtk_icon_theme_load_icon(gtk_icon_theme_get_default(),
                                 BALSA_PIXMAP_IDENTITY, 48,
                                 GTK_ICON_LOOKUP_USE_BUILTIN, NULL);
    if (!pod->pixbuf) {
        gchar *conttype = libbalsa_message_body_get_mime_type(body);

        pod->pixbuf = libbalsa_icon_finder(NULL, conttype, NULL, NULL, 48);
    }
    pod->c_image_width  = gdk_pixbuf_get_width(pod->pixbuf);
    pod->c_image_height = gdk_pixbuf_get_height(pod->pixbuf);


    /* create a layout for calculating the maximum label width */
    header_font =
        pango_font_description_from_string(balsa_app.print_header_font);
    test_layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_font_description(test_layout, header_font);
    pango_font_description_free(header_font);

    /* add fields from the address */
    desc_buf           = g_string_new("");
    pod->p_label_width = 0;
    ADD_VCARD_FIELD(desc_buf, pod->p_label_width, test_layout,
                    libbalsa_address_get_full_name(address), _("Full Name"));
    ADD_VCARD_FIELD(desc_buf, pod->p_label_width, test_layout,
                    libbalsa_address_get_nick_name(address), _("Nick Name"));
    ADD_VCARD_FIELD(desc_buf, pod->p_label_width, test_layout,
                    libbalsa_address_get_first_name(address), _("First Name"));
    ADD_VCARD_FIELD(desc_buf, pod->p_label_width, test_layout,
                    libbalsa_address_get_last_name(address), _("Last Name"));
    ADD_VCARD_FIELD(desc_buf, pod->p_label_width, test_layout,
                    libbalsa_address_get_organization(address), _("Organization"));
    ADD_VCARD_FIELD(desc_buf, pod->p_label_width, test_layout,
                    libbalsa_address_get_addr(address), _("Email Address"));

    g_object_unref(address);

    /* add a small space between label and value */
    pod->p_label_width += C_TO_P(C_LABEL_SEP);

    /* configure the layout so we can calculate the text height */
    pango_layout_set_indent(test_layout, -pod->p_label_width);
    tabs =
        pango_tab_array_new_with_positions(1, FALSE, PANGO_TAB_LEFT,
                                           pod->p_label_width);
    pango_layout_set_tabs(test_layout, tabs);
    pango_tab_array_free(tabs);
    pango_layout_set_width(test_layout,
                           C_TO_P(balsa_print_object_get_c_width(po) -
                                  4 * C_LABEL_SEP - pod->c_image_width));
    pango_layout_set_alignment(test_layout, PANGO_ALIGN_LEFT);
    pod->c_text_height =
        P_TO_C(p_string_height_from_layout(test_layout, desc_buf->str));
    pod->description = g_string_free(desc_buf, FALSE);

    /* check if we should move to the next page */
    c_max_height = MAX(pod->c_text_height, pod->c_image_height);
    if (psetup->c_y_pos + c_max_height > psetup->c_height) {
        psetup->c_y_pos = 0;
        psetup->page_count++;
    }

    /* remember the extent */
    balsa_print_object_set_on_page(po, psetup->page_count - 1);
    balsa_print_object_set_c_at_x(po, psetup->c_x0 + balsa_print_object_get_depth(
                                      po) * C_LABEL_SEP);
    balsa_print_object_set_c_at_y(po, psetup->c_y0 + psetup->c_y_pos);
    balsa_print_object_set_c_width(po, psetup->c_width - 2 * balsa_print_object_get_depth(
                                       po) * C_LABEL_SEP);
    balsa_print_object_set_c_height(po, c_max_height);

    /* adjust the y position */
    psetup->c_y_pos += c_max_height;

    return g_list_append(list, po);
}


/* add a text/calendar object */

#define ADD_VCAL_FIELD(buf, labwidth, layout, field, descr)             \
    do {                                                                \
        if (field) {                                                    \
            gint label_width = p_string_width_from_layout(layout, descr); \
            if (label_width > labwidth)                                 \
                labwidth = label_width;                                 \
            if ((buf)->len > 0)                                         \
                g_string_append_c(buf, '\n');                           \
            g_string_append_printf(buf, "%s\t%s", descr, field);        \
        }                                                               \
    } while (0)

#define ADD_VCAL_DATE(buf, labwidth, layout, date, descr)               \
    do {                                                                \
        if (date != (time_t) -1) {                                      \
            gchar *_dstr =                                             \
                libbalsa_date_to_utf8(date, balsa_app.date_string);     \
            ADD_VCAL_FIELD(buf, labwidth, layout, _dstr, descr);        \
            g_free(_dstr);                                              \
        }                                                               \
    } while (0)

#define ADD_VCAL_ADDRESS(buf, labwidth, layout, addr, descr)            \
    do {                                                                \
        if (addr) {                                                     \
            gchar *_astr = libbalsa_vcal_attendee_to_str(addr);        \
            ADD_VCAL_FIELD(buf, labwidth, layout, _astr, descr);        \
            g_free(_astr);                                              \
        }                                                               \
    } while (0)


GList *
balsa_print_object_calendar(GList               *list,
                            GtkPrintContext     *context,
                            LibBalsaMessageBody *body,
                            BalsaPrintSetup     *psetup)
{
    BalsaPrintObjectDefault *pod;
    BalsaPrintObject *po;
    PangoFontDescription *header_font;
    PangoLayout *test_layout;
    PangoTabArray *tabs;
    GString *desc_buf;
    LibBalsaVCal *vcal_obj;
    GList *this_ev;
    guint first_page;
    GList *par_parts;
    GList *this_par_part;
    gdouble c_at_y;

    /* check if we can evaluate the body as calendar object and fall back
     * to text if not */
    if (!(vcal_obj = libbalsa_vcal_new_from_body(body)))
        return balsa_print_object_text(list, context, body, psetup);

    /* proceed with the address information */
    pod = g_object_new(BALSA_TYPE_PRINT_OBJECT_DEFAULT, NULL);
    g_assert(pod != NULL);
    po = BALSA_PRINT_OBJECT(pod);

    /* create the part */
    balsa_print_object_set_depth(po, psetup->curr_depth);
    balsa_print_object_set_c_width(po,
                                   psetup->c_width
                                   - 2 * psetup->curr_depth * C_LABEL_SEP);

    /* get the stock calendar icon or the mime type icon on fail */
    pod->pixbuf =
        gtk_icon_theme_load_icon(gtk_icon_theme_get_default(),
                                 "x-office-calendar", 48,
                                 GTK_ICON_LOOKUP_USE_BUILTIN, NULL);
    if (!pod->pixbuf) {
        gchar *conttype = libbalsa_message_body_get_mime_type(body);

        pod->pixbuf = libbalsa_icon_finder(NULL, conttype, NULL, NULL, 48);
    }
    pod->c_image_width  = gdk_pixbuf_get_width(pod->pixbuf);
    pod->c_image_height = gdk_pixbuf_get_height(pod->pixbuf);

    /* move to the next page if the icon doesn't fit */
    if (psetup->c_y_pos + pod->c_image_height > psetup->c_height) {
        psetup->c_y_pos = 0;
        psetup->page_count++;
    }

    /* create a layout for calculating the maximum label width and for splitting
     * the body (if necessary) */
    header_font =
        pango_font_description_from_string(balsa_app.print_header_font);
    test_layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_font_description(test_layout, header_font);
    pango_font_description_free(header_font);

    /* add fields from the events*/
    desc_buf           = g_string_new("");
    pod->p_label_width = 0;
    for (this_ev = libbalsa_vcal_get_vevent(vcal_obj);
         this_ev != NULL; this_ev = this_ev->next) {
        LibBalsaVEvent *event = (LibBalsaVEvent *) this_ev->data;
        const gchar *description;
        GList *attendee;

        if (desc_buf->len > 0)
            g_string_append_c(desc_buf, '\n');
        ADD_VCAL_FIELD(desc_buf, pod->p_label_width, test_layout,
                       libbalsa_vevent_get_summary(event), _("Summary"));
        ADD_VCAL_ADDRESS(desc_buf, pod->p_label_width, test_layout,
                         libbalsa_vevent_get_organizer(event), _("Organizer"));
        ADD_VCAL_DATE(desc_buf, pod->p_label_width, test_layout,
                      libbalsa_vevent_get_start(event), _("Start"));
        ADD_VCAL_DATE(desc_buf, pod->p_label_width, test_layout,
                      libbalsa_vevent_get_end(event), _("End"));
        ADD_VCAL_FIELD(desc_buf, pod->p_label_width, test_layout,
                       libbalsa_vevent_get_location(event), _("Location"));

        attendee = libbalsa_vevent_get_attendee(event);
        if (attendee != NULL) {
            GList *att = attendee;
            gchar *this_att;

            this_att =
                libbalsa_vcal_attendee_to_str(LIBBALSA_ADDRESS(att->data));
            att = att->next;
            ADD_VCAL_FIELD(desc_buf, pod->p_label_width, test_layout,
                           this_att, att ? _("Attendees") : _("Attendee"));
            g_free(this_att);
            for (; att != NULL; att = att->next) {
                this_att =
                    libbalsa_vcal_attendee_to_str(LIBBALSA_ADDRESS(att->data));
                g_string_append_printf(desc_buf, "\n\t%s", this_att);
                g_free(this_att);
            }
        }

        description = libbalsa_vevent_get_description(event);
        if (description != NULL) {
            gchar **desc_lines = g_strsplit(description, "\n", -1);
            gint i;

            ADD_VCAL_FIELD(desc_buf, pod->p_label_width, test_layout,
                           desc_lines[0], _("Description"));
            for (i = 1; desc_lines[i]; i++) {
                g_string_append_printf(desc_buf, "\n\t%s", desc_lines[i]);
            }
            g_strfreev(desc_lines);
        }
    }
    g_object_unref(vcal_obj);

    /* add a small space between label and value */
    pod->p_label_width += C_TO_P(C_LABEL_SEP);

    /* configure the layout so we can split the text */
    pango_layout_set_indent(test_layout, -pod->p_label_width);
    tabs =
        pango_tab_array_new_with_positions(1, FALSE, PANGO_TAB_LEFT,
                                           pod->p_label_width);
    pango_layout_set_tabs(test_layout, tabs);
    pango_tab_array_free(tabs);
    pango_layout_set_width(test_layout,
                           C_TO_P(balsa_print_object_get_c_width(po) -
                                  4 * C_LABEL_SEP - pod->c_image_width));
    pango_layout_set_alignment(test_layout, PANGO_ALIGN_LEFT);

    /* split paragraph if necessary */
    first_page = psetup->page_count - 1;
    c_at_y     = psetup->c_y_pos;
    par_parts  =
        split_for_layout(test_layout, desc_buf->str, NULL, psetup, TRUE, NULL);
    g_string_free(desc_buf, TRUE);

    /* set the parameters of the first part */
    pod->description   = (gchar *) par_parts->data;
    pod->c_text_height =
        P_TO_C(p_string_height_from_layout(test_layout, pod->description));
    balsa_print_object_set_on_page(po, first_page++);
    balsa_print_object_set_c_at_x(po, psetup->c_x0 + balsa_print_object_get_depth(
                                      po) * C_LABEL_SEP);
    balsa_print_object_set_c_at_y(po, psetup->c_y0 + c_at_y);
    balsa_print_object_set_c_height(po, MAX(pod->c_image_height, pod->c_text_height));
    list = g_list_append(list, pod);

    /* add more parts */
    for (this_par_part = par_parts->next; this_par_part != NULL;
         this_par_part = this_par_part->next) {
        BalsaPrintObjectDefault *new_pod;
        BalsaPrintObject *new_po;

        /* create a new object */
        new_pod = g_object_new(BALSA_TYPE_PRINT_OBJECT_DEFAULT, NULL);
        g_assert(new_pod != NULL);
        new_po = BALSA_PRINT_OBJECT(new_pod);

        /* fill data */
        new_pod->p_label_width = pod->p_label_width;
        new_pod->c_image_width = pod->c_image_width;
        new_pod->description   = (gchar *) this_par_part->data;
        new_pod->c_text_height =
            P_TO_C(p_string_height_from_layout(test_layout, new_pod->description));
        balsa_print_object_set_on_page(new_po, first_page++);
        balsa_print_object_set_c_at_x(new_po, psetup->c_x0 + balsa_print_object_get_depth(
                                          po) * C_LABEL_SEP);
        balsa_print_object_set_c_at_y(new_po, psetup->c_y0);
        balsa_print_object_set_c_height(new_po, new_pod->c_text_height);
        balsa_print_object_set_depth(new_po, psetup->curr_depth);
        balsa_print_object_set_c_width(new_po,
                                       psetup->c_width
                                       - 2 * psetup->curr_depth * C_LABEL_SEP);

        /* append */
        list = g_list_append(list, new_pod);
    }
    g_list_free(par_parts);
    g_object_unref(test_layout);

    return list;
}
