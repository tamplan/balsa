/* -*-mode:c; c-style:k&r; c-basic-offset:4; -*- */
/* Balsa E-Mail Client
 * Copyright (C) 1997-2002 Stuart Parmenter and others,
 *                         See the file AUTHORS for a list.
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  
 * 02111-1307, USA.
 */

#include <string.h>
#include <gtk/gtk.h>
#include "config.h"
#include "balsa-app.h"
#include "print.h"
#include "misc.h"
#include "balsa-message.h"
#include "quote-color.h"
#include "i18n.h"
#include "balsa-print-object.h"
#include "balsa-print-object-decor.h"
#include "balsa-print-object-header.h"


typedef struct {
    GtkWidget *header_font;
    GtkWidget *body_font;
    GtkWidget *footer_font;
    GtkWidget *highlight_cited;
    GtkWidget *highlight_phrases;
} BalsaPrintPrefs;


typedef struct {
    /* related message */
    LibBalsaMessage *message;
    
    /* print setup */
    BalsaPrintSetup setup;

    /* print data */
    GList *print_parts;
 
    /* header related stuff */
    gdouble c_header_y;

    /* page footer related stuff */
    gchar *footer;
    gdouble c_footer_y;
} BalsaPrintData;


/* print the page header and footer */
static void
print_header_footer(GtkPrintContext * context, cairo_t * cairo_ctx,
		    BalsaPrintData * pdata, gint pageno)
{
    PangoLayout *layout;
    PangoFontDescription *font;
    gchar *pagebuf;

    /* page number */
    font = pango_font_description_from_string(balsa_app.print_header_font);
    layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_width(layout, C_TO_P(pdata->setup.c_width));
    pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    pagebuf =
	g_strdup_printf(_("Page %d of %d"), pageno + 1,	pdata->setup.page_count);
    pango_layout_set_text(layout, pagebuf, -1);
    g_free(pagebuf);
    cairo_move_to(cairo_ctx, pdata->setup.c_x0, pdata->c_header_y);
    pango_cairo_show_layout(cairo_ctx, layout);
    g_object_unref(G_OBJECT(layout));

    /* footer (if available) */
    if (pdata->footer) {
	font =
	    pango_font_description_from_string(balsa_app.
					       print_footer_font);
	layout = gtk_print_context_create_pango_layout(context);
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	pango_layout_set_width(layout, C_TO_P(pdata->setup.c_width));
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	pango_layout_set_text(layout, pdata->footer, -1);
	cairo_move_to(cairo_ctx, pdata->setup.c_x0, pdata->c_footer_y);
	pango_cairo_show_layout(cairo_ctx, layout);
	g_object_unref(G_OBJECT(layout));
    }
}


/*
 * scan the body list and prepare print data according to the content type
 */
static GList *
scan_body(GList *bpo_list, GtkPrintContext * context, BalsaPrintSetup * psetup,
	  LibBalsaMessageBody * body, gboolean no_first_sep)
{
#ifdef HAVE_GPGME
    gboolean add_signature;
    gboolean is_multipart_signed;
#endif				/* HAVE_GPGME */

    while (body) {
	gchar *conttype;

	conttype = libbalsa_message_body_get_mime_type(body);
#ifdef HAVE_GPGME
	add_signature = body->sig_info &&
	    g_ascii_strcasecmp(conttype, "application/pgp-signature") &&
	    g_ascii_strcasecmp(conttype, "application/pkcs7-signature") &&
	    g_ascii_strcasecmp(conttype, "application/x-pkcs7-signature");
	if (!g_ascii_strcasecmp("multipart/signed", conttype) &&
	    body->parts && body->parts->next
	    && body->parts->next->sig_info) {
	    is_multipart_signed = TRUE;
	    bpo_list = balsa_print_object_separator(bpo_list, psetup);
	    no_first_sep = TRUE;
	    if (body->was_encrypted)
		bpo_list = balsa_print_object_frame_begin(bpo_list,
							  _("Signed and encrypted matter"),
							  psetup);
	    else
		bpo_list = balsa_print_object_frame_begin(bpo_list,
							  _("Signed matter"),
							  psetup);
	} else
	    is_multipart_signed = FALSE;
#endif				/* HAVE_GPGME */

	if (g_ascii_strncasecmp(conttype, "multipart/", 10)) {
	    if (no_first_sep)
		no_first_sep = FALSE;
	    else
		 bpo_list = balsa_print_object_separator(bpo_list, psetup);
#ifdef HAVE_GPGME
	    if (add_signature) {
		if (body->was_encrypted)
		    bpo_list = balsa_print_object_frame_begin(bpo_list,
							      _("Signed and encrypted matter"),
							      psetup);
		else
		    bpo_list = balsa_print_object_frame_begin(bpo_list,
							      _("Signed matter"),
							      psetup);
	    }
#endif				/* HAVE_GPGME */
	    bpo_list = balsa_print_objects_append_from_body(bpo_list, context,
							    body, psetup);
	}

	if (body->parts) {
	    bpo_list = scan_body(bpo_list, context, psetup, body->parts, no_first_sep);
	    no_first_sep = FALSE;
	}

	/* end the frame for an embedded message */
	if (!g_ascii_strcasecmp(conttype, "message/rfc822")
#ifdef HAVE_GPGME
	    || is_multipart_signed
#endif
	    )
	    bpo_list = balsa_print_object_frame_end(bpo_list, psetup);

#ifdef HAVE_GPGME
	if (add_signature) {
	    gchar *header =
		g_strdup_printf(_("This is an inline %s signed %s message part:"),
				body->sig_info->protocol == GPGME_PROTOCOL_OpenPGP ?
				_("OpenPGP") : _("S/MIME"), conttype);
	    bpo_list = balsa_print_object_header_crypto(bpo_list, context, body, header, psetup);
	    g_free(header);
	    bpo_list = balsa_print_object_frame_end(bpo_list, psetup);
	}
#endif				/* HAVE_GPGME */
	g_free(conttype);

	body = body->next;
    }

    return bpo_list;
}


static void
begin_print(GtkPrintOperation * operation, GtkPrintContext * context,
	    BalsaPrintData * pdata)
{
    GtkPageSetup *page_setup;
    PangoLayout *layout;
    PangoFontDescription *font;
    gchar *pagebuf;
    gchar *subject;
    gchar *date;
    GString *footer_string;

    /* initialise the context */
    page_setup = gtk_print_context_get_page_setup(context);
    // FIXME - make extra margins configurable?
    pdata->setup.c_x0 = 0.0;
    pdata->setup.c_y0 = 0.0;
    pdata->setup.c_width =
	gtk_page_setup_get_page_width(page_setup, GTK_UNIT_POINTS);
    pdata->setup.c_height =
	gtk_page_setup_get_page_height(page_setup, GTK_UNIT_POINTS);
    pdata->setup.page_count = 1;

    /* create a layout so we can do some calculations */
    layout = gtk_print_context_create_pango_layout(context);
    pagebuf = g_strdup_printf(_("Page %d of %d"), 17, 42);

    /* basic body font height */
    font = pango_font_description_from_string(balsa_app.print_body_font);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pdata->setup.p_body_font_height =
	p_string_height_from_layout(layout, pagebuf);

    /* basic header font and header height */
    font = pango_font_description_from_string(balsa_app.print_header_font);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pdata->setup.p_hdr_font_height =
	p_string_height_from_layout(layout, pagebuf);
    g_free(pagebuf);

    pdata->c_header_y = pdata->setup.c_y0;
    pdata->setup.c_y0 += P_TO_C(pdata->setup.p_hdr_font_height) + C_HEADER_SEP;
    pdata->setup.c_y_pos = pdata->setup.c_y0;
    pdata->setup.c_height -=
	P_TO_C(pdata->setup.p_hdr_font_height) + C_HEADER_SEP;

    /* now create the footer string so we can reduce the height accordingly */
    subject = g_strdup(LIBBALSA_MESSAGE_GET_SUBJECT(pdata->message));
    libbalsa_utf8_sanitize(&subject, balsa_app.convert_unknown_8bit, NULL);
    if (subject)
	footer_string = g_string_new(subject);
    else
	footer_string = NULL;

    date = libbalsa_message_date_to_utf8(pdata->message, balsa_app.date_string);
    if (footer_string) {
	footer_string = g_string_append(footer_string, " \342\200\224 ");
	footer_string = g_string_append(footer_string, date);
    } else
	footer_string = g_string_new(date);
    g_free(date);

    if (pdata->message->headers->from) {
	gchar *from =
	    internet_address_list_to_string(pdata->message->headers->from, FALSE);

	libbalsa_utf8_sanitize(&from, balsa_app.convert_unknown_8bit,
			       NULL);
	if (footer_string) {
	    footer_string =
		g_string_prepend(footer_string, " \342\200\224 ");
	    footer_string = g_string_prepend(footer_string, from);
	} else {
	    footer_string = g_string_new(from);
	}
	g_free(from);
    }

    /* if a footer is available, remember the string and adjust the height */
    if (footer_string) {
	gint p_height;

	/* create a layout to calculate the height of the footer */
	font = pango_font_description_from_string(balsa_app.print_footer_font);
	pango_layout_set_font_description(layout, font);
	pango_font_description_free(font);
	pango_layout_set_width(layout, C_TO_P(pdata->setup.c_width));
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

	/* calculate the height and adjust the printable region */
	p_height = p_string_height_from_layout(layout, footer_string->str);
	pdata->c_footer_y =
	    pdata->setup.c_y0 + pdata->setup.c_height - P_TO_C(p_height);
	pdata->setup.c_height -= P_TO_C(p_height) + C_HEADER_SEP;

	/* remember in the context */
	pdata->footer = footer_string->str;
	g_string_free(footer_string, FALSE);
    }
    g_object_unref(G_OBJECT(layout));

    /* add the message headers */
    pdata->setup.c_y_pos = 0.0;	/* to simplify calculating the layout... */
    pdata->print_parts = 
	balsa_print_object_header_from_message(NULL, context, pdata->message,
					       subject, &pdata->setup);
    g_free(subject);

    /* add the mime bodies */
    pdata->print_parts = 
	scan_body(pdata->print_parts, context, &pdata->setup,
		  pdata->message->body_list, FALSE);

    /* done */
    gtk_print_operation_set_n_pages(operation, pdata->setup.page_count);
}


static void
draw_page(GtkPrintOperation * operation, GtkPrintContext * context,
	  gint page_nr, BalsaPrintData * print_data)
{
    cairo_t *cairo_ctx;
    GList * p;

    g_message("print page %d of %d", page_nr + 1, print_data->setup.page_count);

    /* emit a warning if we try to print a non-existing page */
    if (page_nr >= print_data->setup.page_count) {
	balsa_information(LIBBALSA_INFORMATION_WARNING,
			  _("Cannot print page %d because the document has only %d pages."),
			  page_nr + 1, print_data->setup.page_count);
	return;
    }

    /* get the cairo context */
    cairo_ctx = gtk_print_context_get_cairo_context(context);

    /* print the page header and footer */
    print_header_footer(context, cairo_ctx, print_data, page_nr);

    /* print parts */
    p = print_data->print_parts;
    while (p) {
	BalsaPrintObject *po = BALSA_PRINT_OBJECT(p->data);

	if (po->on_page == page_nr)
	    balsa_print_object_draw(po, context, cairo_ctx);

	p = g_list_next(p);
    }
}

/* setup gui related stuff */
static GtkWidget *
add_font_button(const gchar * text, const gchar * font, GtkTable * table,
		gint row)
{
    GtkWidget *label;
    GtkWidget *font_button;

    label = gtk_label_new(text);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_table_attach(table, label, 0, 1, row, row + 1, GTK_FILL, 0, 0, 0);

    font_button = gtk_font_button_new_with_font(font);
    gtk_table_attach(table, font_button, 1, 2, row, row + 1,
		     GTK_EXPAND | GTK_FILL,
		     (GtkAttachOptions) (GTK_FILL), 0, 0);

    return font_button;
}


static GtkWidget *
message_prefs_widget(GtkPrintOperation * operation,
		     BalsaPrintPrefs * print_prefs)
{
    GtkWidget *page;
    GtkWidget *group;
    GtkWidget *label;
    GtkWidget *hbox;
    GtkWidget *vbox;
    GtkWidget *table;
    gchar *markup;

    gtk_print_operation_set_custom_tab_label(operation, _("Message"));

    page = gtk_vbox_new(FALSE, 18);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);

    group = gtk_vbox_new(FALSE, 12);
    gtk_box_pack_start(GTK_BOX(page), group, FALSE, TRUE, 0);

    label = gtk_label_new(NULL);
    markup = g_strdup_printf("<b>%s</b>", _("Fonts"));
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(group), label, FALSE, FALSE, 0);

    hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(group), hbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("    "),
		       FALSE, FALSE, 0);
    vbox = gtk_vbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    table = gtk_table_new(3, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 6);
    gtk_table_set_col_spacings(GTK_TABLE(table), 6);

    gtk_box_pack_start(GTK_BOX(vbox), table, FALSE, TRUE, 0);

    print_prefs->header_font =
	add_font_button(_("Header Font:"), balsa_app.print_header_font,
			GTK_TABLE(table), 0);
    print_prefs->body_font =
	add_font_button(_("Body Font:"), balsa_app.print_body_font,
			GTK_TABLE(table), 1);
    print_prefs->footer_font =
	add_font_button(_("Footer Font:"), balsa_app.print_footer_font,
			GTK_TABLE(table), 2);

    group = gtk_vbox_new(FALSE, 12);
    gtk_box_pack_start(GTK_BOX(page), group, FALSE, TRUE, 0);

    label = gtk_label_new(NULL);
    markup = g_strdup_printf("<b>%s</b>", _("Highlighting"));
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(group), label, FALSE, FALSE, 0);

    hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(group), hbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("    "),
		       FALSE, FALSE, 0);
    vbox = gtk_vbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    print_prefs->highlight_cited =
	gtk_check_button_new_with_mnemonic(_("Highlight _cited text"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON
				 (print_prefs->highlight_cited),
				 balsa_app.print_highlight_cited);
    gtk_box_pack_start(GTK_BOX(vbox), print_prefs->highlight_cited, FALSE,
		       TRUE, 0);

    print_prefs->highlight_phrases =
	gtk_check_button_new_with_mnemonic(_
					   ("Highlight _structured phrases"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON
				 (print_prefs->highlight_phrases),
				 balsa_app.print_highlight_phrases);
    gtk_box_pack_start(GTK_BOX(vbox), print_prefs->highlight_phrases,
		       FALSE, TRUE, 0);

    gtk_widget_show_all(page);

    return page;
}


static void
message_prefs_apply(GtkPrintOperation * operation, GtkWidget * widget,
		    BalsaPrintPrefs * print_prefs)
{
    g_free(balsa_app.print_header_font);
    balsa_app.print_header_font =
	g_strdup(gtk_font_button_get_font_name
		 (GTK_FONT_BUTTON(print_prefs->header_font)));
    g_free(balsa_app.print_body_font);
    balsa_app.print_body_font =
	g_strdup(gtk_font_button_get_font_name
		 (GTK_FONT_BUTTON(print_prefs->body_font)));
    g_free(balsa_app.print_footer_font);
    balsa_app.print_footer_font =
	g_strdup(gtk_font_button_get_font_name
		 (GTK_FONT_BUTTON(print_prefs->footer_font)));
    balsa_app.print_highlight_cited =
	GTK_TOGGLE_BUTTON(print_prefs->highlight_cited)->active;
    balsa_app.print_highlight_phrases =
	GTK_TOGGLE_BUTTON(print_prefs->highlight_phrases)->active;
}


void
message_print_page_setup(GtkWindow * parent)
{
    GtkPageSetup *new_page_setup;

    if (!balsa_app.print_settings)
	balsa_app.print_settings = gtk_print_settings_new();

    new_page_setup =
	gtk_print_run_page_setup_dialog(parent, balsa_app.page_setup,
					balsa_app.print_settings);

    if (balsa_app.page_setup)
	g_object_unref(G_OBJECT(balsa_app.page_setup));

    balsa_app.page_setup = new_page_setup;
}


void
message_print(LibBalsaMessage * msg, GtkWindow * parent)
{
    GtkPrintOperation *print;
    GtkPrintOperationResult res;
    BalsaPrintData *print_data;
    BalsaPrintPrefs print_prefs;
    GError *err = NULL;

    print = gtk_print_operation_new();
    g_assert(print != NULL);

    g_object_ref(G_OBJECT(msg));

    gtk_print_operation_set_n_pages(print, 1);
    gtk_print_operation_set_unit(print, GTK_UNIT_POINTS);
    gtk_print_operation_set_use_full_page(print, FALSE);

    if (balsa_app.print_settings != NULL)
	gtk_print_operation_set_print_settings(print,
					       balsa_app.print_settings);
    if (balsa_app.page_setup != NULL)
	gtk_print_operation_set_default_page_setup(print,
						   balsa_app.page_setup);

    /* create a print context */
    print_data = g_new0(BalsaPrintData, 1);
    print_data->message = msg;

    g_signal_connect(print, "begin_print", G_CALLBACK(begin_print), print_data);
    g_signal_connect(print, "draw_page", G_CALLBACK(draw_page), print_data);
    g_signal_connect(print, "create-custom-widget",
		     G_CALLBACK(message_prefs_widget), &print_prefs);
    g_signal_connect(print, "custom-widget-apply",
		     G_CALLBACK(message_prefs_apply), &print_prefs);

    res =
	gtk_print_operation_run(print,
				GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
				parent, &err);

    if (res == GTK_PRINT_OPERATION_RESULT_APPLY) {
	if (balsa_app.print_settings != NULL)
	    g_object_unref(balsa_app.print_settings);
	balsa_app.print_settings =
	    g_object_ref(gtk_print_operation_get_print_settings(print));
    } else if (res == GTK_PRINT_OPERATION_RESULT_ERROR)
	balsa_information(LIBBALSA_INFORMATION_ERROR,
			  _("Error printing message: %s"), err->message);

    /* clean up */
    if (err)
	g_error_free(err);
    g_list_foreach(print_data->print_parts, (GFunc) g_object_unref, NULL);
    g_list_free(print_data->print_parts);
    g_free(print_data->footer);
    g_free(print_data);
    g_object_unref(G_OBJECT(print));
    g_object_unref(G_OBJECT(msg));
}