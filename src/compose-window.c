/* -*-mode:c; c-style:k&r; c-basic-offset:4; -*- */
/* Balsa E-Mail Client
 * Copyright (C) 1998-2016 Stuart Parmenter and others, see AUTHORS file.
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* FONT SELECTION DISCUSSION:
   We use pango now.
   Locale data is then used exclusively for the spelling checking.
 */


#if defined(HAVE_CONFIG_H) && HAVE_CONFIG_H
#   include "config.h"
#endif                          /* HAVE_CONFIG_H */
#include "sendmsg-window.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define GNOME_PAD_SMALL    4
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <ctype.h>
#include <glib.h>

#ifdef HAVE_LOCALE_H
#   include <locale.h>
#endif

#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif
#include <errno.h>
#include "application-helpers.h"
#include "identity-widgets.h"
#include "libbalsa.h"
#include "misc.h"
#include "send.h"
#include "html.h"

#include "balsa-app.h"
#include "balsa-message.h"
#include "balsa-index.h"
#include "balsa-icons.h"

#include "missing.h"
#include "ab-window.h"
#include "address-view.h"
#include "print.h"
#include "macosx-helpers.h"

#if !HAVE_GSPELL && !HAVE_GTKSPELL_3_0_3
#   include <enchant/enchant.h>
#endif                          /* HAVE_GTKSPELL_3_0_3 */
#if HAVE_GTKSPELL
#   include "gtkspell/gtkspell.h"
#elif HAVE_GSPELL
#   include "gspell/gspell.h"
#else                           /* HAVE_GTKSPELL */
#   include "spell-check.h"
#endif                          /* HAVE_GTKSPELL */
#if HAVE_GTKSOURCEVIEW
#   include <gtksourceview/gtksource.h>
#endif                          /* HAVE_GTKSOURCEVIEW */

typedef enum {
    SENDMSG_STATE_CLEAN,
    SENDMSG_STATE_MODIFIED,
    SENDMSG_STATE_AUTO_SAVED
} SendmsgState;

struct _BalsaComposeWindow {
    GtkApplicationWindow app_window;

    GtkWidget           *toolbar;
    LibBalsaAddressView *recipient_view, *replyto_view;
    GtkWidget           *from[2], *recipients[2], *subject[2], *fcc[2];
    GtkWidget           *replyto[2];
    GtkWidget           *tree_view;
    gchar               *in_reply_to;
    GList               *references;
    GtkWidget           *text;
#if !HAVE_GTKSPELL
    GtkWidget *spell_checker;
#endif                          /* HAVE_GTKSPELL */
    GtkWidget       *notebook;
    LibBalsaMessage *parent_message;     /* to which we're replying     */
    LibBalsaMessage *draft_message;      /* where the message was saved */
    SendType         type;
    gboolean         is_continue;
    /* language selection related data */
    gchar     *spell_check_lang;
    GtkWidget *current_language_menu;
    /* identity related data */
    LibBalsaIdentity *ident;
    /* fcc mailbox */
    gchar   *fcc_url;
    gboolean update_config;     /* is the window being set up or in normal  */
                                /* operation and user actions should update */
                                /* the config */
    gulong delete_sig_id;
    gulong changed_sig_id;
#if !HAVE_GTKSOURCEVIEW
    gulong delete_range_sig_id;
#endif                          /* HAVE_GTKSOURCEVIEW */
    gulong       insert_text_sig_id;
    guint        autosave_timeout_id;
    SendmsgState state;
    gulong       identities_changed_id;
    gboolean     flow;          /* send format=flowed */
    gboolean     send_mp_alt;   /* send multipart/alternative (plain and html) */
    gboolean     req_mdn;       /* send a MDN */
    gboolean     req_dsn;       /* send a delivery status notification */
    gboolean     quit_on_close; /* quit balsa after the compose window */
                                /* is closed.                          */
#ifdef HAVE_GPGME
    guint    gpg_mode;
    gboolean attach_pubkey;
#endif

#if !HAVE_GTKSOURCEVIEW
    GtkTextBuffer *buffer2;     /* Undo buffer. */
#endif                          /* HAVE_GTKSOURCEVIEW */

    /* To update cursor after text is inserted. */
    GtkTextMark *insert_mark;

    GtkWidget *paned;
    gboolean   ready_to_send;
    gboolean   have_weak_ref;
};

G_DEFINE_TYPE(BalsaComposeWindow, balsa_compose_window, GTK_TYPE_APPLICATION_WINDOW);

static void balsa_compose_window_destroy(GtkWidget *widget);
static void balsa_compose_window_size_allocate(GtkWidget           *widget,
                                               const GtkAllocation *allocation,
                                               int                  baseline);
static gboolean balsa_compose_window_close_request(GtkWindow *window);

static void
balsa_compose_window_class_init(BalsaComposeWindowClass *klass)
{
    GtkWidgetClass *widget_class;
    GtkWindowClass *window_class;

    widget_class = (GtkWidgetClass *) klass;
    window_class = (GtkWindowClass *) klass;

    widget_class->destroy = balsa_compose_window_destroy;
    widget_class->size_allocate = balsa_compose_window_size_allocate;

    window_class->close_request = balsa_compose_window_close_request;
}

static void
balsa_compose_window_init(BalsaComposeWindow *compose_window)
{
    compose_window->in_reply_to      = NULL;
    compose_window->references       = NULL;
    compose_window->spell_check_lang = NULL;
    compose_window->fcc_url          = NULL;
    compose_window->insert_mark      = NULL;
    compose_window->update_config    = FALSE;
    compose_window->quit_on_close    = FALSE;
    compose_window->state            = SENDMSG_STATE_CLEAN;
    compose_window->type        = SEND_NORMAL;
    compose_window->is_continue = FALSE;
#if !HAVE_GTKSPELL && !HAVE_GSPELL
    compose_window->spell_checker = NULL;
#endif                          /* HAVE_GTKSPELL */
#ifdef HAVE_GPGME
    compose_window->gpg_mode      = LIBBALSA_PROTECT_RFC3156;
    compose_window->attach_pubkey = FALSE;
#endif
    compose_window->draft_message  = NULL;
    compose_window->parent_message = NULL;
    compose_window->req_mdn = FALSE;
    compose_window->req_dsn = FALSE;
    compose_window->send_mp_alt = FALSE;

    gtk_window_set_role(GTK_WINDOW(compose_window), "compose");
}

typedef struct {
    pid_t         pid_editor;
    gchar        *filename;
    BalsaComposeWindow *compose_window;
} balsa_edit_with_gnome_data;

typedef enum {
    QUOTE_HEADERS, QUOTE_ALL, QUOTE_NOPREFIX
} QuoteType;

static gint message_postpone(BalsaComposeWindow *compose_window);

static void check_readiness(BalsaComposeWindow *compose_window);
static void init_menus(BalsaComposeWindow *compose_window);

#ifdef HAVE_GPGME
static void compose_window_setup_gpg_ui(BalsaComposeWindow *compose_window);
static void compose_window_update_gpg_ui_on_ident_change(BalsaComposeWindow     *compose_window,
                                                LibBalsaIdentity *new_ident);
static void compose_window_setup_gpg_ui_by_mode(BalsaComposeWindow *compose_window,
                                       gint          mode);

#endif

#if !HAVE_GSPELL && !HAVE_GTKSPELL_3_0_3
static void sw_spell_check_weak_notify(BalsaComposeWindow *compose_window);

#endif                          /* HAVE_GTKSPELL */

static void address_book_cb(LibBalsaAddressView *address_view,
                            GtkWidget           *widget,
                            BalsaComposeWindow        *compose_window);
static void address_book_response(GtkWidget           *ab,
                                  gint                 response,
                                  LibBalsaAddressView *address_view);

static void set_locale(BalsaComposeWindow *compose_window,
                       const gchar  *locale);

static void replace_identity_signature(BalsaComposeWindow     *compose_window,
                                       LibBalsaIdentity *new_ident,
                                       LibBalsaIdentity *old_ident,
                                       gint             *replace_offset,
                                       gint              siglen,
                                       const gchar      *new_sig);
static void update_compose_window_identity(BalsaComposeWindow *compose_window,
                                  LibBalsaIdentity *identity);

static GString *quote_message_body(BalsaComposeWindow    *compose_window,
                                   LibBalsaMessage *message,
                                   QuoteType        type);
static void         set_list_post_address(BalsaComposeWindow *compose_window);
static gboolean     set_list_post_rfc2369(BalsaComposeWindow *compose_window,
                                          const gchar  *url);
static const gchar *rfc2822_skip_comments(const gchar *str);
static void         balsa_compose_window_set_title(BalsaComposeWindow *compose_window);

#if !HAVE_GTKSOURCEVIEW
/* Undo/Redo buffer helpers. */
static void sw_buffer_save(BalsaComposeWindow *compose_window);
static void sw_buffer_swap(BalsaComposeWindow *compose_window,
                           gboolean      undo);

#endif                          /* HAVE_GTKSOURCEVIEW */
static void sw_buffer_signals_connect(BalsaComposeWindow *compose_window);

#if !HAVE_GTKSOURCEVIEW || !(HAVE_GSPELL || HAVE_GTKSPELL)
static void sw_buffer_signals_disconnect(BalsaComposeWindow *compose_window);

#endif                          /* !HAVE_GTKSOURCEVIEW || !HAVE_GTKSPELL */
#if !HAVE_GTKSOURCEVIEW
static void sw_buffer_set_undo(BalsaComposeWindow *compose_window,
                               gboolean      undo,
                               gboolean      redo);

#endif                          /* HAVE_GTKSOURCEVIEW */

/* Standard DnD types */
enum {
    TARGET_MESSAGES,
    TARGET_URI_LIST,
    TARGET_STRING
};

static const gchar *drop_types[] = {
    "x-application/x-message-list",
    "text/uri-list",
    "STRING",
    "text/plain"
};

static const gchar *email_field_drop_types[] = {
    "STRING",
    "text/plain"
};

static void lang_set_cb(GtkWidget    *widget,
                        BalsaComposeWindow *compose_window);

static void compose_window_set_subject_from_body(BalsaComposeWindow        *compose_window,
                                        LibBalsaMessageBody *body,
                                        LibBalsaIdentity    *ident);

/* the array of locale names and charset names included in the MIME
   type information.
   if you add a new encoding here add to SendCharset in libbalsa.c
 */
struct SendLocales {
    const gchar *locale, *charset, *lang_name;
} locales[] =
    /* Translators: please use the initial letter of each language as
     * its accelerator; this is a long list, and unique accelerators
     * cannot be found. */
{
    {"pt_BR",        "ISO-8859-1",         N_("_Brazilian Portuguese")                                   },
    {"ca_ES",        "ISO-8859-15",        N_("_Catalan")                                                },
    {"zh_CN.GB2312", "gb2312",             N_("_Chinese Simplified")                                     },
    {"zh_TW.Big5",   "big5",               N_("_Chinese Traditional")                                    },
    {"cs_CZ",        "ISO-8859-2",         N_("_Czech")                                                  },
    {"da_DK",        "ISO-8859-1",         N_("_Danish")                                                 },
    {"nl_NL",        "ISO-8859-15",        N_("_Dutch")                                                  },
    {"en_US",        "ISO-8859-1",         N_("_English (American)")                                     },
    {"en_GB",        "ISO-8859-1",         N_("_English (British)")                                      },
    {"eo_XX",        "UTF-8",              N_("_Esperanto")                                              },
    {"et_EE",        "ISO-8859-15",        N_("_Estonian")                                               },
    {"fi_FI",        "ISO-8859-15",        N_("_Finnish")                                                },
    {"fr_FR",        "ISO-8859-15",        N_("_French")                                                 },
    {"de_DE",        "ISO-8859-15",        N_("_German")                                                 },
    {"de_AT",        "ISO-8859-15",        N_("_German (Austrian)")                                      },
    {"de_CH",        "ISO-8859-1",         N_("_German (Swiss)")                                         },
    {"el_GR",        "ISO-8859-7",         N_("_Greek")                                                  },
    {"he_IL",        "UTF-8",              N_("_Hebrew")                                                 },
    {"hu_HU",        "ISO-8859-2",         N_("_Hungarian")                                              },
    {"it_IT",        "ISO-8859-15",        N_("_Italian")                                                },
    {"ja_JP",        "ISO-2022-JP",        N_("_Japanese (JIS)")                                         },
    {"kk_KZ",        "UTF-8",              N_("_Kazakh")                                                 },
    {"ko_KR",        "euc-kr",             N_("_Korean")                                                 },
    {"lv_LV",        "ISO-8859-13",        N_("_Latvian")                                                },
    {"lt_LT",        "ISO-8859-13",        N_("_Lithuanian")                                             },
    {"no_NO",        "ISO-8859-1",         N_("_Norwegian")                                              },
    {"pl_PL",        "ISO-8859-2",         N_("_Polish")                                                 },
    {"pt_PT",        "ISO-8859-15",        N_("_Portuguese")                                             },
    {"ro_RO",        "ISO-8859-2",         N_("_Romanian")                                               },
    {"ru_RU",        "KOI8-R",             N_("_Russian")                                                },
    {"sr_Cyrl",      "ISO-8859-5",         N_("_Serbian")                                                },
    {"sr_Latn",      "ISO-8859-2",         N_("_Serbian (Latin)")                                        },
    {"sk_SK",        "ISO-8859-2",         N_("_Slovak")                                                 },
    {"es_ES",        "ISO-8859-15",        N_("_Spanish")                                                },
    {"sv_SE",        "ISO-8859-1",         N_("_Swedish")                                                },
    {"tt_RU",        "UTF-8",              N_("_Tatar")                                                  },
    {"tr_TR",        "ISO-8859-9",         N_("_Turkish")                                                },
    {"uk_UK",        "KOI8-U",             N_("_Ukrainian")                                              },
    {"",             "UTF-8",              N_("_Generic UTF-8")                                          }
};

static const gchar *
sw_preferred_charset(BalsaComposeWindow *compose_window)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(locales); i++) {
        if (g_strcmp0(compose_window->spell_check_lang, locales[i].locale) == 0)
            return locales[i].charset;
    }

    return NULL;
}


/* ===================================================================
   Balsa menus. Touchpad has some simplified menus which do not
   overlap very much with the default balsa menus. They are here
   because they represent an alternative probably appealing to the all
   proponents of GNOME2 dumbify approach (OK, I am bit unfair here).
 */

/*
 * lists of actions that are enabled or disabled as groups
 */
static const gchar *const ready_actions[] = {
    "send", "queue", "postpone"
};

/* ===================================================================
 *                attachment related stuff
 * =================================================================== */

enum {
    ATTACH_INFO_COLUMN = 0,
    ATTACH_ICON_COLUMN,
    ATTACH_TYPE_COLUMN,
    ATTACH_MODE_COLUMN,
    ATTACH_SIZE_COLUMN,
    ATTACH_DESC_COLUMN,
    ATTACH_NUM_COLUMNS
};

static const gchar *const attach_modes[] =
{
    NULL, N_("Attachment"), N_("Inline"), N_("Reference")
};

struct _BalsaAttachInfo {
    GObject parent_object;

    BalsaComposeWindow *bm;                 /* send message back reference */

    GtkWidget              *popup_menu;        /* popup menu */
    LibbalsaVfs            *file_uri;          /* file uri of the attachment */
    gchar                  *uri_ref;           /* external body URI reference */
    gchar                  *force_mime_type;   /* force using this particular mime type */
    gchar                  *charset;           /* forced character set */
    gboolean                delete_on_destroy; /* destroy the file when not used any more */
    gint                    mode;              /* LIBBALSA_ATTACH_AS_ATTACHMENT etc. */
    LibBalsaMessageHeaders *headers;           /* information about a forwarded message */
};


static void balsa_attach_info_finalize(GObject *object);


#define BALSA_MSG_ATTACH_MODEL(x)   gtk_tree_view_get_model(GTK_TREE_VIEW((x)->tree_view))


#define BALSA_TYPE_ATTACH_INFO balsa_attach_info_get_type()

G_DECLARE_FINAL_TYPE(BalsaAttachInfo,
                     balsa_attach_info,
                     BALSA,
                     ATTACH_INFO,
                     GObject);

G_DEFINE_TYPE(BalsaAttachInfo,
              balsa_attach_info,
              G_TYPE_OBJECT)

static void
balsa_attach_info_class_init(BalsaAttachInfoClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = balsa_attach_info_finalize;
}


static void
balsa_attach_info_init(BalsaAttachInfo *info)
{
    info->popup_menu        = NULL;
    info->file_uri          = NULL;
    info->force_mime_type   = NULL;
    info->charset           = NULL;
    info->delete_on_destroy = FALSE;
    info->mode              = LIBBALSA_ATTACH_AS_ATTACHMENT;
    info->headers           = NULL;
}


static BalsaAttachInfo *
balsa_attach_info_new(BalsaComposeWindow *bm)
{
    BalsaAttachInfo *info = g_object_new(BALSA_TYPE_ATTACH_INFO, NULL);

    info->bm = bm;
    return info;
}


static void
balsa_attach_info_finalize(GObject *object)
{
    BalsaAttachInfo *info;

    g_return_if_fail(object != NULL);
    g_return_if_fail(BALSA_IS_ATTACH_INFO(object));
    info = BALSA_ATTACH_INFO(object);

    /* unlink the file if necessary */
    if (info->delete_on_destroy && info->file_uri) {
        gchar *folder_name;

        /* unlink the file */
        if (balsa_app.debug)
            fprintf (stderr, "%s:%s: unlink `%s'\n", __FILE__, __FUNCTION__,
                     libbalsa_vfs_get_uri_utf8(info->file_uri));
        libbalsa_vfs_file_unlink(info->file_uri, NULL);

        /* remove the folder if possible */
        folder_name = g_filename_from_uri(libbalsa_vfs_get_folder(info->file_uri),
                                          NULL, NULL);
        if (folder_name) {
            if (balsa_app.debug)
                fprintf (stderr, "%s:%s: rmdir `%s'\n", __FILE__, __FUNCTION__,
                         folder_name);
            g_rmdir(folder_name);
            g_free(folder_name);
        }
    }

    /* clean up memory */
    if (info->popup_menu)
        gtk_widget_destroy(info->popup_menu);
    if (info->file_uri)
        g_object_unref(G_OBJECT(info->file_uri));
    g_free(info->force_mime_type);
    g_free(info->charset);
    libbalsa_message_headers_destroy(info->headers);

    G_OBJECT_CLASS(balsa_attach_info_parent_class)->finalize(object);
}


/* ===================================================================
 *                end of attachment related stuff
 * =================================================================== */


static void
append_comma_separated(GtkEditable *editable,
                       const gchar *text)
{
    gint position;

    if (!text || !*text)
        return;

    gtk_editable_set_position(editable, -1);
    position = gtk_editable_get_position(editable);
    if (position > 0)
        gtk_editable_insert_text(editable, ", ", 2, &position);
    gtk_editable_insert_text(editable, text, -1, &position);
    gtk_editable_set_position(editable, position);
}


/* the callback handlers */
#define BALSA_SENDMSG_ADDRESS_BOOK_KEY "balsa-sendmsg-address-book"
#define BALSA_SENDMSG_BUTTON_KEY       "balsa-sendmsg-button"
static void
address_book_cb(LibBalsaAddressView *address_view,
                GtkWidget           *widget,
                BalsaComposeWindow        *compose_window)
{
    GtkWidget *ab;

    /* Show only one dialog per window. */
    ab = g_object_get_data(G_OBJECT(compose_window),
                           BALSA_SENDMSG_ADDRESS_BOOK_KEY);
    if (ab != NULL) {
        gtk_window_present(GTK_WINDOW(ab));
        return;
    }

    gtk_widget_set_sensitive(GTK_WIDGET(address_view), FALSE);

    ab = balsa_ab_window_new(TRUE, GTK_WINDOW(compose_window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(ab), TRUE);
    g_signal_connect(G_OBJECT(ab), "response",
                     G_CALLBACK(address_book_response), address_view);
    g_object_set_data_full(G_OBJECT(ab), BALSA_SENDMSG_BUTTON_KEY,
                           g_object_ref(widget), g_object_unref);
    g_object_set_data(G_OBJECT(compose_window),
                      BALSA_SENDMSG_ADDRESS_BOOK_KEY, ab);
    gtk_widget_show(ab);
}


/* Callback for the "response" signal for the address book dialog. */
static void
address_book_response(GtkWidget           *ab,
                      gint                 response,
                      LibBalsaAddressView *address_view)
{
    GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(ab));
    GtkWidget *button =
        g_object_get_data(G_OBJECT(ab), BALSA_SENDMSG_BUTTON_KEY);

    if (response == GTK_RESPONSE_OK) {
        gchar *t = balsa_ab_window_get_recipients(BALSA_AB_WINDOW(ab));
        libbalsa_address_view_add_to_row(address_view, button, t);
        g_free(t);
    }

    gtk_widget_destroy(ab);
    g_object_set_data(G_OBJECT(parent), BALSA_SENDMSG_ADDRESS_BOOK_KEY,
                      NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(address_view), TRUE);
}


static void
sw_delete_draft(BalsaComposeWindow *compose_window)
{
    LibBalsaMessage *message;

    message = compose_window->draft_message;
    if (message != NULL) {
        LibBalsaMailbox *mailbox;

        mailbox = libbalsa_message_get_mailbox(message);
        if ((mailbox != NULL) && !libbalsa_mailbox_get_readonly(mailbox)) {
            libbalsa_message_change_flags(message,
                                          LIBBALSA_MESSAGE_FLAG_DELETED, 0);
        }
    }
}


static gboolean
close_handler(BalsaComposeWindow *compose_window)
{
    InternetAddressList *list;
    InternetAddress *ia;
    const gchar *tmp = NULL;
    gchar *free_me   = NULL;
    gint reply;
    GtkWidget *d;

    if (balsa_app.debug)
        printf("%s\n", __func__);

    if (compose_window->state == SENDMSG_STATE_CLEAN)
        return FALSE;

    list = libbalsa_address_view_get_list(compose_window->recipient_view, "To:");
    ia   = internet_address_list_get_address(list, 0);
    if (ia) {
        tmp = ia->name;
        if (!tmp || !*tmp)
            tmp = free_me = internet_address_to_string(ia, FALSE);
    }
    if (!tmp || !*tmp)
        tmp = _("(No name)");

    d = gtk_message_dialog_new(GTK_WINDOW(compose_window),
                               GTK_DIALOG_DESTROY_WITH_PARENT,
                               GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_YES_NO,
                               _("The message to “%s” is modified.\n"
                                 "Save message to Draftbox?"), tmp);
    g_free(free_me);
#if HAVE_MACOSX_DESKTOP
    libbalsa_macosx_menu_for_parent(d, GTK_WINDOW(compose_window));
#endif
    g_object_unref(list);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_YES);
    gtk_dialog_add_button(GTK_DIALOG(d),
                          _("_Cancel"), GTK_RESPONSE_CANCEL);
    reply = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);

    switch (reply) {
    case GTK_RESPONSE_YES:
        if (compose_window->state == SENDMSG_STATE_MODIFIED)
            if (!message_postpone(compose_window))
                return TRUE;

        break;

    case GTK_RESPONSE_NO:
        if (!compose_window->is_continue)
            sw_delete_draft(compose_window);
        break;

    default:
        return TRUE;
    }

    return FALSE;
}


static gboolean
close_request_cb(GtkWidget *widget,
                 gpointer   data)
{
    BalsaComposeWindow *compose_window = data;

    return close_handler(compose_window);
}


static gboolean
balsa_compose_window_close_request(GtkWindow *window)
{
    BalsaComposeWindow *compose_window = (BalsaComposeWindow *) window;

    return close_handler(compose_window);
}


static void
sw_close_activated(GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    BALSA_DEBUG_MSG("close_window_cb: start\n");
    g_object_set_data(G_OBJECT(compose_window), "destroying",
                      GINT_TO_POINTER(TRUE));
    if (!close_handler(compose_window))
        gtk_widget_destroy((GtkWidget *) compose_window);
    BALSA_DEBUG_MSG("close_window_cb: end\n");
}


/* the balsa_compose_window destructor; copies first the shown headers setting
   to the balsa_app structure.
 */
#define BALSA_SENDMSG_WINDOW_KEY "balsa-sendmsg-window-key"
static void
balsa_compose_window_destroy(GtkWidget *widget)
{
    BalsaComposeWindow *compose_window = (BalsaComposeWindow *) widget;
    gboolean quit_on_close;

    g_assert(compose_window != NULL);

    if (balsa_app.main_window != NULL) {
        if (compose_window->delete_sig_id != 0U) {
            g_signal_handler_disconnect(balsa_app.main_window,
                                        compose_window->delete_sig_id);
            compose_window->delete_sig_id = 0U;
        }
        if (compose_window->identities_changed_id != 0U) {
            g_signal_handler_disconnect(balsa_app.main_window,
                                        compose_window->identities_changed_id);
            compose_window->identities_changed_id = 0U;
        }
        if (compose_window->have_weak_ref) {
            g_object_weak_unref(G_OBJECT(balsa_app.main_window),
                                (GWeakNotify) gtk_widget_destroy,
                                (GtkWidget *) compose_window);
            compose_window->have_weak_ref = FALSE;
        }
    }
    if (balsa_app.debug) g_message("balsa_compose_window_destroy()_handler: Start.");

    if (compose_window->parent_message != NULL) {
        LibBalsaMailbox *mailbox;

        mailbox = libbalsa_message_get_mailbox(compose_window->parent_message);
        if (mailbox != NULL) {
            libbalsa_mailbox_close(mailbox,
                                   /* Respect pref setting: */
                                   balsa_app.expunge_on_close);
        }
        g_clear_object(&compose_window->parent_message);
    }

    if (compose_window->draft_message != NULL) {
        LibBalsaMailbox *mailbox;

        g_object_set_data(G_OBJECT(compose_window->draft_message),
                          BALSA_SENDMSG_WINDOW_KEY, NULL);
        mailbox = libbalsa_message_get_mailbox(compose_window->draft_message);
        if (mailbox != NULL) {
            libbalsa_mailbox_close(mailbox,
                                   /* Respect pref setting: */
                                   balsa_app.expunge_on_close);
        }
        g_clear_object(&compose_window->draft_message);
    }

    if (balsa_app.debug)
        printf("balsa_compose_window_destroy_handler: Freeing compose_window\n");
    gtk_widget_destroy((GtkWidget *) compose_window);
    g_clear_pointer(&compose_window->fcc_url, g_free);
    g_clear_pointer(&compose_window->in_reply_to, g_free);
    libbalsa_clear_list(&compose_window->references, g_free);

#if !(HAVE_GSPELL || HAVE_GTKSPELL)
    g_clear_pointer(&compose_window->spell_checker, gtk_widget_destroy);
#endif                          /* HAVE_GTKSPELL */
    libbalsa_clear_source_id(&compose_window->autosave_timeout_id);

#if !HAVE_GTKSOURCEVIEW
    g_clear_object(&compose_window->buffer2);
#endif                          /* HAVE_GTKSOURCEVIEW */

    /* Move the current identity to the start of the list */
    balsa_app.identities = g_list_remove(balsa_app.identities,
                                         compose_window->ident);
    balsa_app.identities = g_list_prepend(balsa_app.identities,
                                          compose_window->ident);

    g_clear_pointer(&compose_window->spell_check_lang, g_free);

    quit_on_close = compose_window->quit_on_close;

    GTK_WIDGET_CLASS(balsa_compose_window_parent_class)->destroy(widget);

    if (quit_on_close) {
        libbalsa_wait_for_sending_thread(-1);
        gtk_main_quit();
    }
    if (balsa_app.debug) g_message("balsa_compose_window_destroy(): Stop.");
}


/* language menu helper functions */
/* find_locale_index_by_locale:
   finds the longest fit so the one who has en_GB will gent en_US if en_GB
   is not defined.
   NOTE: test for the 'C' locale would not be necessary if people set LANG
   instead of LC_ALL. But it is simpler to set it here instead of answering
   the questions (OTOH, I am afraid that people will start claiming "but
   balsa can recognize my language!" on failures in other software.
 */
static gint
find_locale_index_by_locale(const gchar *locale)
{
    unsigned i, j, maxfit = 0;
    gint maxpos = -1;

    if (locale == NULL || strcmp(locale, "C") == 0)
        locale = "en_US";
    for (i = 0; i < G_N_ELEMENTS(locales); i++) {
        for (j = 0; locale[j] && locales[i].locale[j] == locale[j]; j++) {
        }
        if (j > maxfit) {
            maxfit = j;
            maxpos = i;
        }
    }
    return maxpos;
}


static void
sw_buffer_signals_block(BalsaComposeWindow  *compose_window,
                        GtkTextBuffer *buffer)
{
    g_signal_handler_block(buffer, compose_window->changed_sig_id);
#if !HAVE_GTKSOURCEVIEW
    g_signal_handler_block(buffer, compose_window->delete_range_sig_id);
#endif                          /* HAVE_GTKSOURCEVIEW */
    g_signal_handler_block(buffer, compose_window->insert_text_sig_id);
}


static void
sw_buffer_signals_unblock(BalsaComposeWindow  *compose_window,
                          GtkTextBuffer *buffer)
{
    g_signal_handler_unblock(buffer, compose_window->changed_sig_id);
#if !HAVE_GTKSOURCEVIEW
    g_signal_handler_unblock(buffer, compose_window->delete_range_sig_id);
#endif                          /* HAVE_GTKSOURCEVIEW */
    g_signal_handler_unblock(buffer, compose_window->insert_text_sig_id);
}


static const gchar *const address_types[] =
{
    N_("To:"), N_("CC:"), N_("BCC:")
};

static gboolean
edit_with_gnome_check(gpointer data)
{
    FILE *tmp;
    balsa_edit_with_gnome_data *data_real = (balsa_edit_with_gnome_data *)data;
    GtkTextBuffer *buffer;

    pid_t pid;
    gchar line[81]; /* FIXME:All lines should wrap at this line */
    /* Editor not ready */
    pid = waitpid (data_real->pid_editor, NULL, WNOHANG);
    if (pid == -1) {
        perror("waitpid");
        return TRUE;
    } else if (pid == 0) {
        return TRUE;
    }

    tmp = fopen(data_real->filename, "r");
    if (tmp == NULL) {
        perror("fopen");
        return TRUE;
    }
    if (balsa_app.edit_headers) {
        while (fgets(line, sizeof(line), tmp)) {
            guint type;

            if (line[strlen(line) - 1] == '\n')
                line[strlen(line) - 1] = '\0';

            if (libbalsa_str_has_prefix(line, _("Subject:")) == 0) {
                gtk_entry_set_text(GTK_ENTRY(data_real->compose_window->subject[1]),
                                   line + strlen(_("Subject:")) + 1);
                continue;
            }

            for (type = 0;
                 type < G_N_ELEMENTS(address_types);
                 type++) {
                const gchar *type_string = _(address_types[type]);
                if (libbalsa_str_has_prefix(line, type_string)) {
                    libbalsa_address_view_set_from_string
                        (data_real->compose_window->recipient_view,
                        address_types[type],
                        line + strlen(type_string) + 1);
                }
            }
        }
    }
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(data_real->compose_window->text));

#if !HAVE_GTKSOURCEVIEW
    sw_buffer_save(data_real->compose_window);
#endif                          /* HAVE_GTKSOURCEVIEW */
    sw_buffer_signals_block(data_real->compose_window, buffer);
    gtk_text_buffer_set_text(buffer, "", 0);
    while (fgets(line, sizeof(line), tmp)) {
        gtk_text_buffer_insert_at_cursor(buffer, line, -1);
    }
    sw_buffer_signals_unblock(data_real->compose_window, buffer);

    fclose(tmp);
    unlink(data_real->filename);
    g_free(data_real->filename);
    gtk_widget_set_sensitive(data_real->compose_window->text, TRUE);
    g_free(data);

    return FALSE;
}


/* Edit the current file with an external editor.
 *
 * We fork twice current process, so we get:
 *
 * - Old (parent) process (this needs to continue because we don't want
 *   balsa to 'hang' until the editor exits
 * - New (child) process (forks and waits for child to finish)
 * - New (grandchild) process (executes editor)
 */
static void
sw_edit_activated(GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       data)
{
    BalsaComposeWindow *compose_window             = data;
    static const char TMP_PATTERN[] = "/tmp/balsa-edit-XXXXXX";
    gchar filename[sizeof(TMP_PATTERN)];
    balsa_edit_with_gnome_data *edit_data;
    pid_t pid;
    FILE *tmp;
    int tmpfd;
    GtkTextBuffer *buffer;
    GtkTextIter start, end;
    gchar *p;
    GAppInfo *app;
    char **argv;
    int argc;

    app = g_app_info_get_default_for_type("text/plain", FALSE);
    if (!app) {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_ERROR,
                                   _("GNOME editor is not defined"
                                     " in your preferred applications."));
        return;
    }

    argc    = 2;
    argv    = g_new0 (char *, argc + 1);
    argv[0] = g_strdup(g_app_info_get_executable(app));
    strcpy(filename, TMP_PATTERN);
    argv[1] =
        g_strdup_printf("%s%s",
                        g_app_info_supports_uris(app) ? "file://" : "",
                        filename);
    /* FIXME: how can I detect if the called application needs the
     * terminal??? */
    g_object_unref(app);

    tmpfd = mkstemp(filename);
    tmp   = fdopen(tmpfd, "w+");

    if (balsa_app.edit_headers) {
        guint type;

        fprintf(tmp, "%s %s\n", _("Subject:"),
                gtk_entry_get_text(GTK_ENTRY(compose_window->subject[1])));
        for (type = 0; type < G_N_ELEMENTS(address_types); type++) {
            InternetAddressList *list =
                libbalsa_address_view_get_list(compose_window->recipient_view,
                                               address_types[type]);
            gchar *addr_string = internet_address_list_to_string(list, FALSE);
            g_object_unref(list);
            fprintf(tmp, "%s %s\n", _(address_types[type]), addr_string);
            g_free(addr_string);
        }
        fprintf(tmp, "\n");
    }

    gtk_widget_set_sensitive(GTK_WIDGET((GtkWidget *) compose_window->text), FALSE);
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    p = gtk_text_iter_get_text(&start, &end);
    fputs(p, tmp);
    g_free(p);
    fclose(tmp);
    if ((pid = fork()) < 0) {
        perror ("fork");
        g_strfreev(argv);
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        execvp (argv[0], argv);
        perror ("execvp");
        g_strfreev (argv);
        exit(127);
    }
    g_strfreev (argv);
    /* Return immediately. We don't want balsa to 'hang' */
    edit_data             = g_malloc(sizeof(balsa_edit_with_gnome_data));
    edit_data->pid_editor = pid;
    edit_data->filename   = g_strdup(filename);
    edit_data->compose_window      = compose_window;
    g_timeout_add(200, (GSourceFunc)edit_with_gnome_check, edit_data);
}


static void
sw_select_ident_activated(GSimpleAction *action,
                          GVariant      *parameter,
                          gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    libbalsa_identity_select_dialog(GTK_WINDOW(compose_window),
                                    _("Select Identity"),
                                    balsa_app.identities,
                                    compose_window->ident,
                                    ((LibBalsaIdentityCallback)
                                     update_compose_window_identity),
                                    compose_window);
}


/* NOTE: replace_offset and siglen are  utf-8 character offsets. */
static void
replace_identity_signature(BalsaComposeWindow     *compose_window,
                           LibBalsaIdentity *new_ident,
                           LibBalsaIdentity *old_ident,
                           gint             *replace_offset,
                           gint              siglen,
                           const gchar      *new_sig)
{
    gint newsiglen;
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
    GtkTextIter ins, end;
    GtkTextMark *mark;
    gboolean insert_signature;

    /* Save cursor */
    gtk_text_buffer_get_iter_at_mark(buffer, &ins,
                                     gtk_text_buffer_get_insert(buffer));
    mark = gtk_text_buffer_create_mark(buffer, NULL, &ins, TRUE);

    gtk_text_buffer_get_iter_at_offset(buffer, &ins,
                                       *replace_offset);
    gtk_text_buffer_get_iter_at_offset(buffer, &end,
                                       *replace_offset + siglen);
    gtk_text_buffer_delete(buffer, &ins, &end);

    newsiglen = strlen(new_sig);

    switch (compose_window->type) {
    case SEND_NORMAL:
    default:
        insert_signature = TRUE;
        break;

    case SEND_REPLY:
    case SEND_REPLY_ALL:
    case SEND_REPLY_GROUP:
        insert_signature = libbalsa_identity_get_sig_whenreply(new_ident);
        break;

    case SEND_FORWARD_ATTACH:
    case SEND_FORWARD_INLINE:
        insert_signature = libbalsa_identity_get_sig_whenforward(new_ident);
        break;
    }
    if (insert_signature) {
        gboolean new_sig_prepend = libbalsa_identity_get_sig_prepend(new_ident);
        gboolean old_sig_prepend = libbalsa_identity_get_sig_prepend(old_ident);

        /* see if sig location is probably going to be the same */
        if (new_sig_prepend == old_sig_prepend) {
            /* account for sig length difference in replacement offset */
            *replace_offset += newsiglen - siglen;
        } else if (new_sig_prepend) {
            /* sig location not the same between idents, take a WAG and
             * put it at the start of the message */
            gtk_text_buffer_get_start_iter(buffer, &ins);
            *replace_offset += newsiglen;
        } else {
            /* put it at the end of the message */
            gtk_text_buffer_get_end_iter(buffer, &ins);
        }

        gtk_text_buffer_place_cursor(buffer, &ins);
        gtk_text_buffer_insert_at_cursor(buffer, new_sig, -1);
    }

    /* Restore cursor */
    gtk_text_buffer_get_iter_at_mark(buffer, &ins, mark);
    gtk_text_buffer_place_cursor(buffer, &ins);
    gtk_text_buffer_delete_mark(buffer, mark);
}


/*
 * GAction helpers
 */

static GAction *
sw_get_action(BalsaComposeWindow *compose_window,
              const gchar  *action_name)
{
    GAction *action;

    if (g_object_get_data(G_OBJECT(compose_window), "destroying"))
        return NULL;

    action = g_action_map_lookup_action(G_ACTION_MAP(compose_window),
                                        action_name);
    if (!action)
        g_print("%s %s not found\n", __func__, action_name);

    return action;
}


static void
sw_action_set_enabled(BalsaComposeWindow *compose_window,
                      const gchar  *action_name,
                      gboolean      enabled)
{
    GAction *action;

    action = sw_get_action(compose_window, action_name);
    if (action)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}


/*
 * Enable or disable a group of actions
 */

static void
sw_actions_set_enabled(BalsaComposeWindow       *compose_window,
                       const gchar *const *actions,
                       guint               n_actions,
                       gboolean            enabled)
{
    guint i;

    for (i = 0; i < n_actions; i++) {
        sw_action_set_enabled(compose_window, *actions++, enabled);
    }
}


#if !HAVE_GTKSOURCEVIEW || HAVE_GSPELL || HAVE_GTKSPELL
static gboolean
sw_action_get_enabled(BalsaComposeWindow *compose_window,
                      const gchar  *action_name)
{
    GAction *action;

    action = sw_get_action(compose_window, action_name);
    return action ? g_action_get_enabled(action) : FALSE;
}


#endif                          /* HAVE_GTKSOURCEVIEW */

/* Set the state of a toggle-type GAction. */
static void
sw_action_set_active(BalsaComposeWindow *compose_window,
                     const gchar  *action_name,
                     gboolean      state)
{
    GAction *action;

    action = sw_get_action(compose_window, action_name);
    if (action)
        g_action_change_state(action, g_variant_new_boolean(state));
}


static gboolean
sw_action_get_active(BalsaComposeWindow *compose_window,
                     const gchar  *action_name)
{
    GAction *action;
    gboolean retval = FALSE;

    action = sw_get_action(compose_window, action_name);
    if (action) {
        GVariant *state;

        state  = g_action_get_state(action);
        retval = g_variant_get_boolean(state);
        g_variant_unref(state);
    }

    return retval;
}


/*
 * end of GAction helpers
 */

/*
 * update_compose_window_identity
 *
 * Change the specified BalsaComposeWindow current identity, and update the
 * corresponding fields.
 * */
static void
update_compose_window_identity(BalsaComposeWindow     *compose_window,
                      LibBalsaIdentity *ident)
{
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
    GtkTextIter start, end;

    gint replace_offset = 0;
    gint siglen;

    gboolean found_sig = FALSE;
    gchar *old_sig;
    gchar *new_sig;
    gchar *message_text;
    gchar *compare_str;
    gchar **message_split;
    gchar *tmpstr;
    const gchar *subject;
    gint replen, fwdlen;
    const gchar *addr;
    const gchar *reply_string;
    const gchar *old_reply_string;
    const gchar *forward_string;
    const gchar *old_forward_string;

    LibBalsaIdentity *old_ident;
    gboolean reply_type = (compose_window->type == SEND_REPLY ||
                           compose_window->type == SEND_REPLY_ALL ||
                           compose_window->type == SEND_REPLY_GROUP);
    gboolean forward_type = (compose_window->type == SEND_FORWARD_ATTACH ||
                             compose_window->type == SEND_FORWARD_INLINE);

    g_return_if_fail(ident != NULL);


    /* change entries to reflect new identity */
    gtk_combo_box_set_active(GTK_COMBO_BOX(compose_window->from[1]),
                             g_list_index(balsa_app.identities, ident));

    addr = libbalsa_identity_get_replyto(ident);
    if ((addr != NULL) && (addr[0] != '\0')) {
        libbalsa_address_view_set_from_string(compose_window->replyto_view,
                                              "Reply To:",
                                              addr);
        gtk_widget_show((GtkWidget *) compose_window->replyto[0]);
        gtk_widget_show((GtkWidget *) compose_window->replyto[1]);
    } else if (!sw_action_get_active(compose_window, "reply-to")) {
        gtk_widget_hide((GtkWidget *) compose_window->replyto[0]);
        gtk_widget_hide((GtkWidget *) compose_window->replyto[1]);
    }

    addr = libbalsa_identity_get_bcc(compose_window->ident);
    if (addr != NULL) {
        InternetAddressList *bcc_list, *ident_list;

        bcc_list =
            libbalsa_address_view_get_list(compose_window->recipient_view, "BCC:");

        ident_list = internet_address_list_parse_string(addr);
        if (ident_list) {
            /* Remove any Bcc addresses that came from the old identity
             * from the list. */
            gint ident_list_len = internet_address_list_length(ident_list);
            gint i;

            for (i = 0; i < internet_address_list_length(bcc_list); i++) {
                InternetAddress *ia =
                    internet_address_list_get_address (bcc_list, i);
                gint j;

                for (j = 0; j < ident_list_len; j++) {
                    InternetAddress *ia2 =
                        internet_address_list_get_address(ident_list, j);
                    if (libbalsa_ia_rfc2821_equal(ia, ia2))
                        break;
                }

                if (j < ident_list_len) {
                    /* This address was found in the identity. */
                    internet_address_list_remove_at(bcc_list, i);
                    --i;
                }
            }
            g_object_unref(ident_list);
        }

        /* Add the new Bcc addresses, if any: */
        addr       = libbalsa_identity_get_bcc(ident);
        ident_list = internet_address_list_parse_string(addr);
        if (ident_list) {
            internet_address_list_append(bcc_list, ident_list);
            g_object_unref(ident_list);
        }

        /* Set the resulting list: */
        libbalsa_address_view_set_from_list(compose_window->recipient_view, "BCC:",
                                            bcc_list);
        g_object_unref(bcc_list);
    }

    /* change the subject to use the reply/forward strings */
    subject = gtk_entry_get_text(GTK_ENTRY(compose_window->subject[1]));

    /*
     * If the subject begins with the old reply string
     *    Then replace it with the new reply string.
     * Else, if the subject begins with the old forward string
     *    Then replace it with the new forward string.
     * Else, if the old reply string was empty, and the message
     *    is a reply, OR the old forward string was empty, and the
     *    message is a forward
     *    Then call compose_window_set_subject_from_body()
     * Else assume the user hand edited the subject and does
     *    not want it altered
     */

    reply_string   = libbalsa_identity_get_reply_string(ident);
    forward_string = libbalsa_identity_get_forward_string(ident);

    old_ident          = compose_window->ident;
    old_reply_string   = libbalsa_identity_get_reply_string(old_ident);
    old_forward_string = libbalsa_identity_get_forward_string(old_ident);

    if (((replen = strlen(old_reply_string)) > 0) &&
        g_str_has_prefix(subject, old_reply_string)) {
        tmpstr = g_strconcat(reply_string, &(subject[replen]), NULL);
        gtk_entry_set_text(GTK_ENTRY(compose_window->subject[1]), tmpstr);
        g_free(tmpstr);
    } else if (((fwdlen = strlen(old_forward_string)) > 0) &&
               g_str_has_prefix(subject, old_forward_string)) {
        tmpstr = g_strconcat(forward_string, &(subject[fwdlen]), NULL);
        gtk_entry_set_text(GTK_ENTRY(compose_window->subject[1]), tmpstr);
        g_free(tmpstr);
    } else {
        if (((replen == 0) && reply_type) ||
            ((fwdlen == 0) && forward_type)) {
            LibBalsaMessage *message = compose_window->parent_message != NULL ?
                compose_window->parent_message : compose_window->draft_message;
            compose_window_set_subject_from_body(compose_window,
                                        libbalsa_message_get_body_list(message),
                                        ident);
        }
    }

    /* -----------------------------------------------------------
     * remove/add the signature depending on the new settings, change
     * the signature if path changed */

    /* reconstruct the old signature to search with */
    old_sig = libbalsa_identity_get_signature(old_ident, NULL);

    /* switch identities in compose_window here so we can use read_signature
     * again */
    compose_window->ident = ident;
    if ((reply_type && libbalsa_identity_get_sig_whenreply(ident))
        || (forward_type && libbalsa_identity_get_sig_whenforward(ident))
        || ((compose_window->type == SEND_NORMAL) && libbalsa_identity_get_sig_sending(ident)))
        new_sig = libbalsa_identity_get_signature(ident, NULL);
    else
        new_sig = NULL;
    if (!new_sig) new_sig = g_strdup("");

    gtk_text_buffer_get_bounds(buffer, &start, &end);
    message_text = gtk_text_iter_get_text(&start, &end);
    if (!old_sig) {
        replace_offset = libbalsa_identity_get_sig_prepend(compose_window->ident)
            ? 0 : g_utf8_strlen(message_text, -1);
        replace_identity_signature(compose_window, ident, old_ident, &replace_offset,
                                   0, new_sig);
    } else {
        /* split on sig separator */
        message_split = g_strsplit(message_text, "\n-- \n", 0);
        siglen        = g_utf8_strlen(old_sig, -1);

        /* check the special case of starting a message with a sig */
        compare_str = g_strconcat("\n", message_split[0], NULL);

        if (g_ascii_strncasecmp(old_sig, compare_str, siglen) == 0) {
            g_free(compare_str);
            replace_identity_signature(compose_window, ident, old_ident,
                                       &replace_offset, siglen - 1, new_sig);
            found_sig = TRUE;
        } else {
            gint i;

            g_free(compare_str);
            for (i = 0; message_split[i] != NULL; i++) {
                /* put sig separator back to search */
                compare_str = g_strconcat("\n-- \n", message_split[i], NULL);

                /* try to find occurance of old signature */
                if (g_ascii_strncasecmp(old_sig, compare_str, siglen) == 0) {
                    replace_identity_signature(compose_window, ident, old_ident,
                                               &replace_offset, siglen,
                                               new_sig);
                    found_sig = TRUE;
                }

                replace_offset +=
                    g_utf8_strlen(i ? compare_str : message_split[i], -1);
                g_free(compare_str);
            }
        }
        /* if no sig seperators found, do a slower brute force
         * approach.  We could have stopped earlier if the message was
         * empty, but we didn't. Now, it is really time to do
         * that... */
        if (*message_text && !found_sig) {
            compare_str    = message_text;
            replace_offset = 0;

            /* check the special case of starting a message with a sig */
            tmpstr = g_strconcat("\n", message_text, NULL);

            if (g_ascii_strncasecmp(old_sig, tmpstr, siglen) == 0) {
                g_free(tmpstr);
                replace_identity_signature(compose_window, ident, old_ident,
                                           &replace_offset, siglen - 1,
                                           new_sig);
            } else {
                g_free(tmpstr);
                replace_offset++;
                compare_str = g_utf8_next_char(compare_str);
                while (*compare_str) {
                    if (g_ascii_strncasecmp(old_sig, compare_str, siglen) == 0) {
                        replace_identity_signature(compose_window, ident, old_ident,
                                                   &replace_offset, siglen,
                                                   new_sig);
                    }
                    replace_offset++;
                    compare_str = g_utf8_next_char(compare_str);
                }
            }
        }
        g_strfreev(message_split);
    }
    sw_action_set_active(compose_window, "send-html",
                         libbalsa_identity_get_send_mp_alternative(compose_window->ident));

#ifdef HAVE_GPGME
    compose_window_update_gpg_ui_on_ident_change(compose_window, ident);
#endif

    g_free(old_sig);
    g_free(new_sig);
    g_free(message_text);

    libbalsa_address_view_set_domain(compose_window->recipient_view,
                                     libbalsa_identity_get_domain(ident));

    sw_action_set_active(compose_window, "request-mdn", libbalsa_identity_get_request_mdn(ident));
    sw_action_set_active(compose_window, "request-dsn", libbalsa_identity_get_request_dsn(ident));
}


static void
balsa_compose_window_size_allocate(GtkWidget           *widget,
                                   const GtkAllocation *allocation,
                                   int                  baseline)
{
    GdkSurface *surface;

    GTK_WIDGET_CLASS(balsa_compose_window_parent_class)->size_allocate
        (widget, allocation, baseline);

    surface = gtk_widget_get_surface(widget);
    if (surface == NULL)
        return;

    balsa_app.sw_maximized =
        (gdk_surface_get_state(surface) &
         (GDK_SURFACE_STATE_MAXIMIZED | GDK_SURFACE_STATE_FULLSCREEN)) != 0;

    if (!balsa_app.sw_maximized) {
        gtk_window_get_size(GTK_WINDOW(widget),
                            &balsa_app.sw_width,
                            &balsa_app.sw_height);
    }
}


/* remove_attachment - right mouse button callback */
static void
remove_attachment(GtkWidget       *menu_item,
                  BalsaAttachInfo *info)
{
    GtkTreeIter iter;
    GtkTreeModel *model;
    GtkTreeSelection *selection;
    BalsaAttachInfo *test_info;

    g_return_if_fail(info->bm != NULL);

    /* get the selected element */
    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(info->bm->tree_view));
    if (!gtk_tree_selection_get_selected(selection, &model, &iter))
        return;

    /* make sure we got the right element */
    gtk_tree_model_get(model, &iter, ATTACH_INFO_COLUMN, &test_info, -1);
    if (test_info != info) {
        if (test_info)
            g_object_unref(test_info);
        return;
    }
    g_object_unref(test_info);

    /* remove the attachment */
    gtk_list_store_remove(GTK_LIST_STORE(model), &iter);
}


static void
set_attach_menu_sensitivity(GtkWidget *widget,
                            gpointer   data)
{
    gint mode =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "new-mode"));

    if (mode)
        gtk_widget_set_sensitive(widget, mode != GPOINTER_TO_INT(data));
}


/* change attachment mode - right mouse button callback */
static void
change_attach_mode(GtkWidget       *menu_item,
                   BalsaAttachInfo *info)
{
    gint new_mode =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu_item),
                                          "new-mode"));
    GtkTreeIter iter;
    GtkTreeModel *model;
    GtkTreeSelection *selection;
    BalsaAttachInfo *test_info;

    g_return_if_fail(info->bm != NULL);

    /* get the selected element */
    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(info->bm->tree_view));
    if (!gtk_tree_selection_get_selected(selection, &model, &iter))
        return;

    /* make sure we got the right element */
    gtk_tree_model_get(model, &iter, ATTACH_INFO_COLUMN, &test_info, -1);
    if (test_info != info) {
        if (test_info)
            g_object_unref(test_info);
        return;
    }
    g_object_unref(test_info);

    /* verify that the user *really* wants to attach as reference */
    if ((info->mode != new_mode) && (new_mode == LIBBALSA_ATTACH_AS_EXTBODY)) {
        GtkWidget *extbody_dialog, *parent;
        gint result;

        parent         = gtk_widget_get_toplevel(menu_item);
        extbody_dialog =
            gtk_message_dialog_new(GTK_WINDOW(parent),
                                   GTK_DIALOG_DESTROY_WITH_PARENT,
                                   GTK_MESSAGE_QUESTION,
                                   GTK_BUTTONS_YES_NO,
                                   _("Saying yes will not send the file "
                                     "“%s” itself, but just a MIME "
                                     "message/external-body reference. "
                                     "Note that the recipient must "
                                     "have proper permissions to see the "
                                     "“real” file.\n\n"
                                     "Do you really want to attach "
                                     "this file as reference?"),
                                   libbalsa_vfs_get_uri_utf8(info->file_uri));
#if HAVE_MACOSX_DESKTOP
        libbalsa_macosx_menu_for_parent(extbody_dialog, GTK_WINDOW(parent));
#endif
        gtk_window_set_title(GTK_WINDOW(extbody_dialog),
                             _("Attach as Reference?"));
        result = gtk_dialog_run(GTK_DIALOG(extbody_dialog));
        gtk_widget_destroy(extbody_dialog);
        if (result != GTK_RESPONSE_YES)
            return;
    }

    /* change the attachment mode */
    info->mode = new_mode;
    gtk_list_store_set(GTK_LIST_STORE(model), &iter, ATTACH_MODE_COLUMN,
                       info->mode, -1);

    /* set the menu's sensitivities */
    gtk_container_forall(GTK_CONTAINER(gtk_widget_get_parent(menu_item)),
                         set_attach_menu_sensitivity,
                         GINT_TO_POINTER(info->mode));
}


/* attachment vfs menu - right mouse button callback */
static void
attachment_menu_vfs_cb(GtkWidget       *menu_item,
                       BalsaAttachInfo *info)
{
    GError *err = NULL;
    gboolean result;

    g_return_if_fail(info != NULL);

    result = libbalsa_vfs_launch_app(info->file_uri,
                                     G_OBJECT(menu_item),
                                     &err);
    if (!result) {
        balsa_information(LIBBALSA_INFORMATION_WARNING,
                          _("Could not launch application: %s"),
                          err ? err->message : "Unknown error");
    }
    g_clear_error(&err);
}


/* URL external body - right mouse button callback */
static void
on_open_url_cb(GtkWidget       *menu_item,
               BalsaAttachInfo *info)
{
    GtkWidget *toplevel;
    GError *err = NULL;
    const gchar *uri;

    g_return_if_fail(info != NULL);
    uri = libbalsa_vfs_get_uri(info->file_uri);
    g_return_if_fail(uri != NULL);

    g_message("open URL %s", uri);
    toplevel = gtk_widget_get_toplevel(GTK_WIDGET(menu_item));
    if (gtk_widget_is_toplevel(toplevel)) {
        gtk_show_uri_on_window(GTK_WINDOW(toplevel), uri,
                               gtk_get_current_event_time(), &err);
    }
    if (err) {
        balsa_information(LIBBALSA_INFORMATION_WARNING,
                          _("Error showing %s: %s\n"),
                          uri, err->message);
        g_error_free(err);
    }
}


static GtkWidget *sw_attachment_list(BalsaComposeWindow *compose_window);

static void
show_attachment_widget(BalsaComposeWindow *compose_window)
{
    GtkPaned *outer_paned;
    GtkWidget *child;

    outer_paned = GTK_PANED(compose_window->paned);
    child       = gtk_paned_get_child1(outer_paned);

    if (!GTK_IS_PANED(child)) {
        gint position;
        GtkRequisition minimum_size;
        GtkWidget *paned;
        GtkPaned *inner_paned;

        position = gtk_paned_get_position(outer_paned);
        if (position <= 0) {
            gtk_widget_get_preferred_size(child, &minimum_size, NULL);
            position = minimum_size.height;
        }
        gtk_container_remove(GTK_CONTAINER(compose_window->paned),
                             g_object_ref(child));

        paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

        inner_paned = GTK_PANED(paned);
        gtk_paned_add1(inner_paned, child);
        g_object_unref(child);

        child = sw_attachment_list(compose_window);
        gtk_paned_add2(inner_paned, child);
        gtk_paned_set_position(inner_paned, position);

        gtk_widget_get_preferred_size(child, &minimum_size, NULL);
        gtk_paned_add1(outer_paned, paned);
        gtk_paned_set_position(outer_paned,
                               position + minimum_size.height);
    }
}


/* Ask the user for a charset; returns ((LibBalsaCodeset) -1) on cancel. */
static void
sw_charset_combo_box_changed(GtkComboBox *combo_box,
                             GtkWidget   *charset_button)
{
    gtk_widget_set_sensitive(charset_button,
                             gtk_combo_box_get_active(combo_box) == 0);
}


static LibBalsaCodeset
sw_get_user_codeset(BalsaComposeWindow *compose_window,
                    gboolean     *change_type,
                    const gchar  *mime_type,
                    const char   *fname)
{
    GtkWidget *combo_box = NULL;
    gint codeset         = -1;
    GtkWidget *dialog    =
        gtk_dialog_new_with_buttons(_("Choose character set"),
                                    GTK_WINDOW(compose_window),
                                    GTK_DIALOG_DESTROY_WITH_PARENT |
                                    libbalsa_dialog_flags(),
                                    _("_OK"), GTK_RESPONSE_OK,
                                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                                    NULL);
    gchar *msg = g_strdup_printf
            (_("File\n%s\nis not encoded in US-ASCII or UTF-8.\n"
               "Please choose the character set used to encode the file."),
            fname);
    GtkWidget *info           = gtk_label_new(msg);
    GtkWidget *charset_button = libbalsa_charset_button_new();
    GtkBox *content_box;

#if HAVE_MACOSX_DESKTOP
    libbalsa_macosx_menu_for_parent(dialog, GTK_WINDOW(compose_window));
#endif

    g_free(msg);
    content_box = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
    gtk_box_set_spacing(content_box, 5);
    gtk_box_pack_start(content_box, info);
    gtk_widget_set_vexpand(charset_button, TRUE);
    gtk_box_pack_start(content_box, charset_button);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    if (change_type) {
        GtkWidget *label = gtk_label_new(_("Attach as MIME type:"));
        GtkWidget *hbox  = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        combo_box = gtk_combo_box_text_new();

        gtk_widget_set_vexpand(hbox, TRUE);
        gtk_box_pack_start(content_box, hbox);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_box_pack_start(GTK_BOX(hbox), label);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_box),
                                       mime_type);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_box),
                                       "application/octet-stream");
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_box), 0);
        g_signal_connect(G_OBJECT(combo_box), "changed",
                         G_CALLBACK(sw_charset_combo_box_changed),
                         charset_button);
        gtk_widget_set_hexpand(combo_box, TRUE);
        gtk_box_pack_start(GTK_BOX(hbox), combo_box);
    }

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        if (change_type)
            *change_type =
                gtk_combo_box_get_active(GTK_COMBO_BOX(combo_box)) != 0;
        if (!change_type || !*change_type)
            codeset = gtk_combo_box_get_active(GTK_COMBO_BOX(charset_button));
    }

    gtk_widget_destroy(dialog);
    return (LibBalsaCodeset) codeset;
}


static gboolean
sw_set_charset(BalsaComposeWindow *compose_window,
               const gchar  *filename,
               const gchar  *content_type,
               gboolean     *change_type,
               gchar       **attach_charset)
{
    const gchar *charset;
    LibBalsaTextAttribute attr;

    attr = libbalsa_text_attr_file(filename);
    if ((gint) attr < 0)
        return FALSE;

    if (attr == 0) {
        charset = "us-ascii";
    } else if (attr & LIBBALSA_TEXT_HI_UTF8) {
        charset = "UTF-8";
    } else {
        LibBalsaCodesetInfo *info;
        LibBalsaCodeset codeset =
            sw_get_user_codeset(compose_window, change_type, content_type, filename);
        if (*change_type)
            return TRUE;

        if (codeset == (LibBalsaCodeset) (-1))
            return FALSE;

        info    = &libbalsa_codeset_info[codeset];
        charset = info->std;
        if (info->win && (attr & LIBBALSA_TEXT_HI_CTRL)) {
            charset = info->win;
            balsa_information_parented(GTK_WINDOW(compose_window),
                                       LIBBALSA_INFORMATION_WARNING,
                                       _("Character set for file %s changed "
                                         "from “%s” to “%s”."), filename,
                                       info->std, info->win);
        }
    }
    *attach_charset = g_strdup(charset);

    return TRUE;
}


static LibBalsaMessageHeaders *
get_fwd_mail_headers(const gchar *mailfile)
{
    int fd;
    GMimeStream *stream;
    GMimeParser *parser;
    GMimeMessage *message;
    LibBalsaMessageHeaders *headers;

    /* try to open the mail file */
    if ((fd = open(mailfile, O_RDONLY)) == -1)
        return NULL;

    if ((stream = g_mime_stream_fs_new(fd)) == NULL) {
        close(fd);
        return NULL;
    }

    /* parse the file */
    parser = g_mime_parser_new();
    g_mime_parser_init_with_stream(parser, stream);
    message = g_mime_parser_construct_message (parser);
    g_object_unref (parser);
    g_object_unref(stream);
    close(fd);

    /* get the headers from the gmime message */
    headers = g_new0(LibBalsaMessageHeaders, 1);
    libbalsa_message_headers_from_gmime(headers, message);
    if (!headers->subject) {
        const gchar *subject = g_mime_message_get_subject(message);

        if (!subject)
            headers->subject = g_strdup(_("(no subject)"));
        else
            headers->subject = g_mime_utils_header_decode_text(subject);
    }
    libbalsa_utf8_sanitize(&headers->subject,
                           balsa_app.convert_unknown_8bit,
                           NULL);

    /* unref the gmime message and return the information */
    g_object_unref(message);
    return headers;
}


/* add_attachment:
   adds given filename (uri format) to the list.
 */
gboolean
add_attachment(BalsaComposeWindow *compose_window,
               const gchar  *filename,
               gboolean      is_a_temp_file,
               const gchar  *forced_mime_type)
{
    LibbalsaVfs *file_uri;
    GtkTreeModel *model;
    GtkTreeIter iter;
    BalsaAttachInfo *attach_data;
    gboolean can_inline, is_fwd_message;
    gchar *content_type = NULL;
    gchar *utf8name;
    GError *err = NULL;
    GdkPixbuf *pixbuf;
    GtkWidget *menu_item;
    gchar *content_desc;

    if (balsa_app.debug)
        fprintf(stderr, "Trying to attach '%s'\n", filename);
    if (!(file_uri = libbalsa_vfs_new_from_uri(filename))) {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_ERROR,
                                   _("Cannot create file URI object for %s"),
                                   filename);
        return FALSE;
    }
    if (!libbalsa_vfs_is_regular_file(file_uri, &err)) {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_ERROR,
                                   "%s: %s", filename,
                                   err && err->message ? err->message : _("unknown error"));
        g_error_free(err);
        g_object_unref(file_uri);
        return FALSE;
    }

    /* get the pixbuf for the attachment's content type */
    is_fwd_message = forced_mime_type &&
        !g_ascii_strncasecmp(forced_mime_type, "message/", 8) && is_a_temp_file;
    if (is_fwd_message)
        content_type = g_strdup(forced_mime_type);
    pixbuf =
        libbalsa_icon_finder(GTK_WIDGET(compose_window), forced_mime_type,
                             file_uri, &content_type, 24);
    if (!content_type)
        /* Last ditch. */
        content_type = g_strdup("application/octet-stream");

    /* create a new attachment info block */
    attach_data          = balsa_attach_info_new(compose_window);
    attach_data->charset = NULL;
    if (!g_ascii_strncasecmp(content_type, "text/", 5)) {
        gboolean change_type = FALSE;
        if (!sw_set_charset(compose_window, filename, content_type,
                            &change_type, &attach_data->charset)) {
            g_free(content_type);
            g_object_unref(attach_data);
            return FALSE;
        }
        if (change_type) {
            forced_mime_type = "application/octet-stream";
            g_free(content_type);
            content_type = g_strdup(forced_mime_type);
        }
    }

    if (is_fwd_message) {
        attach_data->headers = get_fwd_mail_headers(filename);
        if (!attach_data->headers) {
            utf8name = g_strdup(_("forwarded message"));
        } else {
            gchar *tmp =
                internet_address_list_to_string(attach_data->headers->from,
                                                FALSE);
            utf8name = g_strdup_printf(_("Message from %s, subject: “%s”"),
                                       tmp,
                                       attach_data->headers->subject);
            g_free(tmp);
        }
    } else {
        const gchar *uri_utf8 = libbalsa_vfs_get_uri_utf8(file_uri);
        const gchar *home     = g_getenv("HOME");

        if (home != NULL && g_str_has_prefix(uri_utf8, "file://") &&
            g_str_has_prefix(uri_utf8 + 7, home))
            utf8name = g_strdup_printf("~%s", uri_utf8 + 7 + strlen(home));
        else
            utf8name = g_strdup(uri_utf8);
    }

    show_attachment_widget(compose_window);

    model = BALSA_MSG_ATTACH_MODEL(compose_window);
    gtk_list_store_append(GTK_LIST_STORE(model), &iter);

    attach_data->file_uri        = file_uri;
    attach_data->force_mime_type = g_strdup(forced_mime_type);

    attach_data->delete_on_destroy = is_a_temp_file;
    can_inline                     = !is_a_temp_file &&
        (!g_ascii_strncasecmp(content_type, "text/", 5) ||
         !g_ascii_strncasecmp(content_type, "image/", 6));
    attach_data->mode = LIBBALSA_ATTACH_AS_ATTACHMENT;

    /* build the attachment's popup menu */
    attach_data->popup_menu = gtk_menu_new();

    /* only real text/... and image/... parts may be inlined */
    if (can_inline) {
        menu_item =
            gtk_menu_item_new_with_label(_(attach_modes
                                           [LIBBALSA_ATTACH_AS_INLINE]));
        g_object_set_data(G_OBJECT(menu_item), "new-mode",
                          GINT_TO_POINTER(LIBBALSA_ATTACH_AS_INLINE));
        g_signal_connect(G_OBJECT(menu_item), "activate",
                         G_CALLBACK(change_attach_mode),
                         (gpointer)attach_data);
        gtk_menu_shell_append(GTK_MENU_SHELL(attach_data->popup_menu),
                              menu_item);
    }

    /* all real files can be attachments */
    if (can_inline || !is_a_temp_file) {
        menu_item =
            gtk_menu_item_new_with_label(_(attach_modes
                                           [LIBBALSA_ATTACH_AS_ATTACHMENT]));
        gtk_widget_set_sensitive(menu_item, FALSE);
        g_object_set_data(G_OBJECT(menu_item), "new-mode",
                          GINT_TO_POINTER(LIBBALSA_ATTACH_AS_ATTACHMENT));
        g_signal_connect(G_OBJECT(menu_item), "activate",
                         G_CALLBACK(change_attach_mode),
                         (gpointer)attach_data);
        gtk_menu_shell_append(GTK_MENU_SHELL(attach_data->popup_menu),
                              menu_item);
    }

    /* real files may be references (external body) */
    if (!is_a_temp_file) {
        menu_item =
            gtk_menu_item_new_with_label(_(attach_modes
                                           [LIBBALSA_ATTACH_AS_EXTBODY]));
        g_object_set_data(G_OBJECT(menu_item), "new-mode",
                          GINT_TO_POINTER(LIBBALSA_ATTACH_AS_EXTBODY));
        g_signal_connect(G_OBJECT(menu_item), "activate",
                         G_CALLBACK(change_attach_mode),
                         (gpointer)attach_data);
        gtk_menu_shell_append(GTK_MENU_SHELL(attach_data->popup_menu),
                              menu_item);
    }

    /* an attachment can be removed */
    menu_item =
        gtk_menu_item_new_with_label(_("Remove"));
    g_signal_connect(G_OBJECT (menu_item), "activate",
                     G_CALLBACK(remove_attachment),
                     (gpointer)attach_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(attach_data->popup_menu),
                          menu_item);

    /* add the usual vfs menu so the user can inspect what (s)he actually
       attached... (only for non-message attachments) */
    if (!is_fwd_message) {
        libbalsa_vfs_fill_menu_by_content_type(GTK_MENU(attach_data->popup_menu),
                                               content_type,
                                               G_CALLBACK(attachment_menu_vfs_cb),
                                               (gpointer)attach_data);
    }

    /* append to the list store */
    content_desc = libbalsa_vfs_content_description(content_type);
    gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                       ATTACH_INFO_COLUMN, attach_data,
                       ATTACH_ICON_COLUMN, pixbuf,
                       ATTACH_TYPE_COLUMN, content_desc,
                       ATTACH_MODE_COLUMN, attach_data->mode,
                       ATTACH_SIZE_COLUMN, libbalsa_vfs_get_size(file_uri),
                       ATTACH_DESC_COLUMN, utf8name,
                       -1);
    g_object_unref(attach_data);
    g_object_unref(pixbuf);
    g_free(utf8name);
    g_free(content_type);
    g_free(content_desc);

    return TRUE;
}


/* add_urlref_attachment:
   adds given url as reference to the to the list.
   frees url.
 */
static gboolean
add_urlref_attachment(BalsaComposeWindow *compose_window,
                      gchar        *url)
{
    GtkTreeModel *model;
    GtkTreeIter iter;
    BalsaAttachInfo *attach_data;
    GdkPixbuf *pixbuf;
    GtkWidget *menu_item;

    if (balsa_app.debug)
        fprintf(stderr, "Trying to attach '%s'\n", url);

    /* get the pixbuf for the attachment's content type */
    pixbuf =
        gtk_icon_theme_load_icon(gtk_icon_theme_get_default(),
                                 "go-jump", 16, 0, NULL);

    /* create a new attachment info block */
    attach_data          = balsa_attach_info_new(compose_window);
    attach_data->charset = NULL;

    show_attachment_widget(compose_window);

    model = BALSA_MSG_ATTACH_MODEL(compose_window);
    gtk_list_store_append(GTK_LIST_STORE(model), &iter);

    attach_data->uri_ref           = g_strconcat("URL:", url, NULL);
    attach_data->force_mime_type   = g_strdup("message/external-body");
    attach_data->delete_on_destroy = FALSE;
    attach_data->mode              = LIBBALSA_ATTACH_AS_EXTBODY;

    /* build the attachment's popup menu - may only be removed */
    attach_data->popup_menu = gtk_menu_new();
    menu_item               =
        gtk_menu_item_new_with_label(_("Remove"));
    g_signal_connect(G_OBJECT (menu_item), "activate",
                     G_CALLBACK(remove_attachment),
                     (gpointer)attach_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(attach_data->popup_menu),
                          menu_item);

    /* add a separator and the usual vfs menu so the user can inspect what
       (s)he actually attached... (only for non-message attachments) */
    gtk_menu_shell_append(GTK_MENU_SHELL(attach_data->popup_menu),
                          gtk_separator_menu_item_new());
    menu_item =
        gtk_menu_item_new_with_label(_("Open…"));
    g_signal_connect(G_OBJECT (menu_item), "activate",
                     G_CALLBACK(on_open_url_cb),
                     (gpointer)attach_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(attach_data->popup_menu),
                          menu_item);

    /* append to the list store */
    gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                       ATTACH_INFO_COLUMN, attach_data,
                       ATTACH_ICON_COLUMN, pixbuf,
                       ATTACH_TYPE_COLUMN, _("(URL)"),
                       ATTACH_MODE_COLUMN, attach_data->mode,
                       ATTACH_SIZE_COLUMN, 0,
                       ATTACH_DESC_COLUMN, url,
                       -1);
    g_object_unref(attach_data);
    g_object_unref(pixbuf);
    g_free(url);

    return TRUE;
}


/* attach_dialog_ok:
   processes the attachment file selection. Adds them to the list,
   showing the attachment list, if was hidden.
 */
static void
attach_dialog_response(GtkWidget    *dialog,
                       gint          response,
                       BalsaComposeWindow *compose_window)
{
    GtkFileChooser *fc;
    GSList *files, *list;
    int res = 0;

    g_object_set_data(G_OBJECT(compose_window),
                      "balsa-sendmsg-window-attach-dialog", NULL);

    if (response != GTK_RESPONSE_OK) {
        gtk_widget_destroy(dialog);
        return;
    }

    fc    = GTK_FILE_CHOOSER(dialog);
    files = gtk_file_chooser_get_uris(fc);
    for (list = files; list; list = list->next) {
        if (!add_attachment(compose_window, list->data, FALSE, NULL))
            res++;
        g_free(list->data);
    }

    g_slist_free(files);

    g_free(balsa_app.attach_dir);
    balsa_app.attach_dir = gtk_file_chooser_get_current_folder_uri(fc);

    if (res == 0)
        gtk_widget_destroy(dialog);
}


static GtkFileChooser *
sw_attach_dialog(BalsaComposeWindow *compose_window)
{
    GtkWidget *fsw;
    GtkFileChooser *fc;

    fsw =
        gtk_file_chooser_dialog_new(_("Attach file"),
                                    GTK_WINDOW(compose_window),
                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                                    _("_OK"), GTK_RESPONSE_OK,
                                    NULL);
#if HAVE_MACOSX_DESKTOP
    libbalsa_macosx_menu_for_parent(fsw, GTK_WINDOW(compose_window));
#endif
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(fsw),
                                    libbalsa_vfs_local_only());
    gtk_window_set_destroy_with_parent(GTK_WINDOW(fsw), TRUE);

    fc = GTK_FILE_CHOOSER(fsw);
    gtk_file_chooser_set_select_multiple(fc, TRUE);
    if (balsa_app.attach_dir)
        gtk_file_chooser_set_current_folder_uri(fc, balsa_app.attach_dir);

    g_signal_connect(G_OBJECT(fc), "response",
                     G_CALLBACK(attach_dialog_response), compose_window);

    gtk_widget_show(fsw);

    return fc;
}


/* attach_clicked - menu callback */
static void
sw_attach_file_activated(GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    sw_attach_dialog(compose_window);
}


/* attach_message:
   returns TRUE on success, FALSE on failure.
 */
static gboolean
attach_message(BalsaComposeWindow    *compose_window,
               LibBalsaMessage *message)
{
    gchar *name, *tmp_file_name;

    if (libbalsa_mktempdir(&tmp_file_name) == FALSE)
        return FALSE;

    name = g_strdup_printf("%s/forwarded-message", tmp_file_name);
    g_free(tmp_file_name);

    if (!libbalsa_message_save(message, name)) {
        g_free(name);
        return FALSE;
    }
    tmp_file_name = g_filename_to_uri(name, NULL, NULL);
    g_free(name);
    add_attachment(compose_window, tmp_file_name, TRUE, "message/rfc822");
    g_free(tmp_file_name);
    return TRUE;
}


static void
insert_selected_messages(BalsaComposeWindow *compose_window,
                         QuoteType     type)
{
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
    GtkWidget *index =
        balsa_window_find_current_index(balsa_app.main_window);
    GList *l;

    if (index && (l = balsa_index_selected_list(BALSA_INDEX(index)))) {
        GList *node;

        for (node = l; node != NULL; node = node->next) {
            LibBalsaMessage *message = node->data;
            GString *body            = quote_message_body(compose_window, message, type);
            gtk_text_buffer_insert_at_cursor(buffer, body->str, body->len);
            g_string_free(body, TRUE);
        }
        g_list_free_full(l, g_object_unref);
    }
}


static void
sw_include_messages_activated(GSimpleAction *action,
                              GVariant      *parameter,
                              gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    insert_selected_messages(compose_window, QUOTE_ALL);
}


static void
sw_attach_messages_activated(GSimpleAction *action,
                             GVariant      *parameter,
                             gpointer       data)
{
    BalsaComposeWindow *compose_window = data;
    GtkWidget *index    =
        balsa_window_find_current_index(balsa_app.main_window);

    if (index) {
        GList *node, *l = balsa_index_selected_list(BALSA_INDEX(index));

        for (node = l; node != NULL; node = node->next) {
            LibBalsaMessage *message = node->data;

            if (!attach_message(compose_window, message)) {
                balsa_information_parented(GTK_WINDOW(compose_window),
                                           LIBBALSA_INFORMATION_WARNING,
                                           _("Attaching message failed.\n"
                                             "Possible reason: not enough temporary space"));
                break;
            }
        }
        g_list_free_full(l, g_object_unref);
    }
}


/* attachments_add - attachments field D&D callback */
static GSList *
uri2gslist(const char *uri_list)
{
    GSList *list = NULL;

    while (*uri_list) {
        char *linebreak = strchr(uri_list, 13);
        int length;

        if (!linebreak || (linebreak[1] != '\n'))
            return list;

        length = linebreak - uri_list;

        if (length && (uri_list[0] != '#')) {
            gchar *this_uri = g_strndup(uri_list, length);

            if (this_uri)
                list = g_slist_append(list, this_uri);
        }

        uri_list = linebreak + 2;
    }
    return list;
}


/* Helper: check if the passed parameter contains a valid RFC 2396 URI (leading
 * & trailing whitespaces allowed). Return a newly allocated string with the
 * spaces stripped on success or NULL on fail. Note that the URI may still be
 * malformed. */
static gchar *
rfc2396_uri(const gchar *instr)
{
    gchar *s1, *uri;
    static const gchar *uri_extra = ";/?:@&=+$,-_.!~*'()%";

    /* remove leading and trailing whitespaces */
    uri = g_strchomp(g_strchug(g_strdup(instr)));

    /* check that the string starts with ftp[s]:// or http[s]:// */
    if (g_ascii_strncasecmp(uri, "ftp://", 6) &&
        g_ascii_strncasecmp(uri, "ftps://", 7) &&
        g_ascii_strncasecmp(uri, "http://", 7) &&
        g_ascii_strncasecmp(uri, "https://", 8)) {
        g_free(uri);
        return NULL;
    }

    /* verify that the string contains only valid chars (see rfc 2396) */
    s1 = uri + 6;   /* skip verified beginning */
    while (*s1 != '\0') {
        if (!g_ascii_isalnum(*s1) && !strchr(uri_extra, *s1)) {
            g_free(uri);
            return NULL;
        }
        s1++;
    }

    /* success... */
    return uri;
}


static void
attachments_add(GtkWidget        *widget,
                GdkDragContext   *context,
                GtkSelectionData *selection_data,
                guint32           time,
                BalsaComposeWindow     *compose_window)
{
    const gchar *target;
    gboolean drag_result = TRUE;

    target = gtk_selection_data_get_target(selection_data);
    if (balsa_app.debug)
        printf("attachments_add: target %s\n", target);

    if (target == g_intern_static_string("x-application/x-message-list")) {
        BalsaIndex *index =
            *(BalsaIndex **) gtk_selection_data_get_data(selection_data);
        LibBalsaMailbox *mailbox = balsa_index_get_mailbox(index);
        GArray *selected         = balsa_index_selected_msgnos_new(index);
        guint i;

        for (i = 0; i < selected->len; i++) {
            guint msgno              = g_array_index(selected, guint, i);
            LibBalsaMessage *message =
                libbalsa_mailbox_get_message(mailbox, msgno);
            if (!message)
                continue;

            if (!attach_message(compose_window, message)) {
                balsa_information_parented(GTK_WINDOW(compose_window),
                                           LIBBALSA_INFORMATION_WARNING,
                                           _("Attaching message failed.\n"
                                             "Possible reason: not enough temporary space"));
            }
            g_object_unref(message);
        }
        balsa_index_selected_msgnos_free(index, selected);
    } else if (target == g_intern_static_string("text/uri-list")) {
        GSList *uri_list, *list;

        uri_list = uri2gslist((gchar *) gtk_selection_data_get_data(selection_data));
        for (list = uri_list; list != NULL; list = list->next) {
            add_attachment(compose_window, list->data, FALSE, NULL);
            g_free(list->data);
        }
        g_slist_free(uri_list);
    } else if ((target == g_intern_static_string("STRING")) ||
               (target == g_intern_static_string("text/plain"))) {
        gchar *url = rfc2396_uri((gchar *) gtk_selection_data_get_data(selection_data));

        if (url)
            add_urlref_attachment(compose_window, url);
        else
            drag_result = FALSE;
    }

    gtk_drag_finish(context, drag_result, time);
}


/* to_add - address-view D&D callback; we assume it's a To: address */
static void
to_add(GtkWidget        *widget,
       GdkDragContext   *context,
       GtkSelectionData *selection_data,
       guint32           time)
{
    const gchar *target;
    gboolean drag_result = FALSE;

#ifdef DEBUG
    /* This leaks the name: */
    g_print("%s atom name %s\n", __func__,
            gdk_atom_name(gtk_selection_data_get_target(selection_data)));
#endif

    target = gtk_selection_data_get_target(selection_data);

    if ((target == g_intern_static_string("STRING")) ||
        (target == g_intern_static_string("text/plain"))) {
        const gchar *address;

        address =
            (const gchar *) gtk_selection_data_get_data(selection_data);
        libbalsa_address_view_add_from_string(LIBBALSA_ADDRESS_VIEW(widget), "To:", address);
        drag_result = TRUE;
    }
    gtk_drag_finish(context, drag_result, time);
}


/*
 * static void create_email_or_string_entry()
 *
 * Creates a gtk_label()/entry pair.
 *
 * Input: GtkWidget* grid       - Grid to attach to.
 *        const gchar* label     - Label string.
 *        int y_pos              - position in the grid.
 *        arr                    - arr[1] is the entry widget.
 *
 * Output: GtkWidget* arr[] - arr[0] will be the label widget.
 */

#define BALSA_COMPOSE_ENTRY "balsa-compose-entry"

static void
create_email_or_string_entry(BalsaComposeWindow *compose_window,
                             GtkWidget    *grid,
                             const gchar  *label,
                             int           y_pos,
                             GtkWidget    *arr[])
{
    GtkWidget *mnemonic_widget;

    mnemonic_widget = arr[1];
    if (GTK_IS_FRAME(mnemonic_widget))
        mnemonic_widget = gtk_bin_get_child(GTK_BIN(mnemonic_widget));
    arr[0] = gtk_label_new_with_mnemonic(label);
    gtk_label_set_mnemonic_widget(GTK_LABEL(arr[0]), mnemonic_widget);
    gtk_widget_set_halign(arr[0], GTK_ALIGN_START);
    g_object_set(arr[0], "margin", GNOME_PAD_SMALL, NULL);
    gtk_grid_attach(GTK_GRID(grid), arr[0], 0, y_pos, 1, 1);

    if (!balsa_app.use_system_fonts) {
        gchar *css;
        GtkCssProvider *css_provider;

        gtk_widget_set_name(arr[1], BALSA_COMPOSE_ENTRY);
        css = libbalsa_font_string_to_css(balsa_app.message_font,
                                          BALSA_COMPOSE_ENTRY);

        css_provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css_provider, css, -1);
        g_free(css);

        gtk_style_context_add_provider(gtk_widget_get_style_context(arr[1]),
                                       GTK_STYLE_PROVIDER(css_provider),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css_provider);
    }

    gtk_widget_set_hexpand(arr[1], TRUE);
    gtk_grid_attach(GTK_GRID(grid), arr[1], 1, y_pos, 1, 1);
}


/*
 * static void create_string_entry()
 *
 * Creates a gtk_label()/gtk_entry() pair.
 *
 * Input: GtkWidget* grid       - Grid to attach to.
 *        const gchar* label     - Label string.
 *        int y_pos              - position in the grid.
 *
 * Output: GtkWidget* arr[] - arr[0] will be the label widget.
 *                          - arr[1] will be the entry widget.
 */
static void
create_string_entry(BalsaComposeWindow *compose_window,
                    GtkWidget    *grid,
                    const gchar  *label,
                    int           y_pos,
                    GtkWidget    *arr[])
{
    arr[1] = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(arr[1]), 2048);
    create_email_or_string_entry(compose_window, grid, label, y_pos, arr);
}


/*
 * static void create_email_entry()
 *
 * Creates a gtk_label()/libbalsa_address_view() and button in a grid for
 * e-mail entries, eg. To:.  It also sets up some callbacks in gtk.
 *
 * Input:
 *         BalsaComposeWindow *compose_window  - The send message window
 *         GtkWidget *grid   - grid to insert the widgets into.
 *         int y_pos          - How far down in the grid to put label.
 * On return, compose_window->address_view and compose_window->addresses[1] have been set.
 */

static void
create_email_entry(BalsaComposeWindow         *compose_window,
                   GtkWidget            *grid,
                   int                   y_pos,
                   LibBalsaAddressView **view,
                   GtkWidget           **widget,
                   const gchar          *label,
                   const gchar *const   *types,
                   guint                 n_types)
{
    GtkWidget *scroll;
    GdkContentFormats *formats;

    *view = libbalsa_address_view_new(types, n_types,
                                      balsa_app.address_book_list,
                                      balsa_app.convert_unknown_8bit);

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    /* This is a horrible hack, but we need to make sure that the
     * recipient list is more than one line high: */
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll),
                                               80);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(*view));

    widget[1] = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(widget[1]), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(widget[1]), scroll);

    create_email_or_string_entry(compose_window, grid, _(label), y_pos, widget);

    g_signal_connect(*view, "drag_data_received",
                     G_CALLBACK(to_add), NULL);
    g_signal_connect(*view, "open-address-book",
                     G_CALLBACK(address_book_cb), compose_window);

    formats = gdk_content_formats_new(email_field_drop_types,
                                      G_N_ELEMENTS(email_field_drop_types));
    gtk_drag_dest_set(GTK_WIDGET(*view), GTK_DEST_DEFAULT_ALL,
                      formats,
                      GDK_ACTION_COPY | GDK_ACTION_MOVE);
    gdk_content_formats_unref(formats);

    libbalsa_address_view_set_domain(*view, libbalsa_identity_get_domain(compose_window->ident));
    g_signal_connect_swapped(*view, "view-changed",
                             G_CALLBACK(check_readiness), compose_window);
}


static void
sw_combo_box_changed(GtkComboBox  *combo_box,
                     BalsaComposeWindow *compose_window)
{
    GtkTreeIter iter;

    if (gtk_combo_box_get_active_iter(combo_box, &iter)) {
        LibBalsaIdentity *ident;

        gtk_tree_model_get(gtk_combo_box_get_model(combo_box), &iter,
                           2, &ident, -1);
        update_compose_window_identity(compose_window, ident);
        g_object_unref(ident);
    }
}


static void
create_from_entry(GtkWidget    *grid,
                  BalsaComposeWindow *compose_window)
{
    compose_window->from[1] =
        libbalsa_identity_combo_box(balsa_app.identities, NULL,
                                    G_CALLBACK(sw_combo_box_changed), compose_window);
    create_email_or_string_entry(compose_window, grid, _("F_rom:"), 0, compose_window->from);
}


static void
sw_gesture_pressed_cb(GtkGestureMultiPress *multi_press,
                      gint                  n_press,
                      gdouble               x,
                      gdouble               y,
                      gpointer              user_data)
{
    GtkGesture *gesture;
    const GdkEvent *event;
    GtkTreeView *tree_view;
    GtkTreePath *path;

    gesture = GTK_GESTURE(multi_press);
    event   =
        gtk_gesture_get_last_event(gesture, gtk_gesture_get_last_updated_sequence(gesture));
    g_return_if_fail(event != NULL);
    if (!gdk_event_triggers_context_menu(event))
        return;

    tree_view = GTK_TREE_VIEW(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)));

    if (gtk_tree_view_get_path_at_pos(tree_view, (gint) x, (gint) y,
                                      &path, NULL, NULL, NULL)) {
        GtkTreeIter iter;
        GtkTreeSelection *selection =
            gtk_tree_view_get_selection(tree_view);
        GtkTreeModel *model = gtk_tree_view_get_model(tree_view);

        gtk_tree_selection_unselect_all(selection);
        gtk_tree_selection_select_path(selection, path);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree_view), path, NULL,
                                 FALSE);
        if (gtk_tree_model_get_iter (model, &iter, path)) {
            BalsaAttachInfo *attach_info;

            gtk_tree_model_get(model, &iter, ATTACH_INFO_COLUMN, &attach_info, -1);
            if (attach_info) {
                if (attach_info->popup_menu) {
                    gtk_menu_popup_at_pointer(GTK_MENU(attach_info->popup_menu),
                                              (GdkEvent *) event);
                }
                g_object_unref(attach_info);
            }
        }
        gtk_tree_path_free(path);
    }
}


static gboolean
attachment_popup_cb(GtkWidget *widget,
                    gpointer   user_data)
{
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
    GtkTreeModel *model;
    GtkTreeIter iter;
    BalsaAttachInfo *attach_info;

    if (!gtk_tree_selection_get_selected(selection, &model, &iter))
        return FALSE;

    gtk_tree_model_get(model, &iter, ATTACH_INFO_COLUMN, &attach_info, -1);
    if (attach_info) {
        if (attach_info->popup_menu) {
            gtk_menu_popup_at_widget(GTK_MENU(attach_info->popup_menu),
                                     GTK_WIDGET(widget),
                                     GDK_GRAVITY_CENTER, GDK_GRAVITY_CENTER,
                                     NULL);
        }
        g_object_unref(attach_info);
    }

    return TRUE;
}


static void
render_attach_mode(GtkTreeViewColumn *column,
                   GtkCellRenderer   *cell,
                   GtkTreeModel      *model,
                   GtkTreeIter       *iter,
                   gpointer           data)
{
    gint mode;

    gtk_tree_model_get(model, iter, ATTACH_MODE_COLUMN, &mode, -1);
    g_object_set(cell, "text", _(attach_modes[mode]), NULL);
}


static void
render_attach_size(GtkTreeViewColumn *column,
                   GtkCellRenderer   *cell,
                   GtkTreeModel      *model,
                   GtkTreeIter       *iter,
                   gpointer           data)
{
    gint mode;
    guint64 size;
    gchar *sstr;

    gtk_tree_model_get(model, iter, ATTACH_MODE_COLUMN, &mode,
                       ATTACH_SIZE_COLUMN, &size, -1);
    if (mode == LIBBALSA_ATTACH_AS_EXTBODY)
        sstr = g_strdup("-");
    else
        sstr = g_format_size(size);
    g_object_set(cell, "text", sstr, NULL);
    g_free(sstr);
}


/* create_info_pane
   creates upper panel with the message headers: From, To, ... and
   returns it.
 */
static GtkWidget *
create_info_pane(BalsaComposeWindow *compose_window)
{
    guint row = 0;
    GtkWidget *grid;

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    g_object_set(G_OBJECT(grid), "margin", 6, NULL);

    /* From: */
    create_from_entry(grid, compose_window);

    /* Create the 'Reply To:' entry before the regular recipients, to
     * get the initial focus in the regular recipients*/
#define REPLY_TO_ROW 3
    create_email_entry(compose_window, grid, REPLY_TO_ROW, &compose_window->replyto_view,
                       compose_window->replyto, "R_eply To:", NULL, 0);

    /* To:, Cc:, and Bcc: */
    create_email_entry(compose_window, grid, ++row, &compose_window->recipient_view,
                       compose_window->recipients, "Rec_ipients:", address_types,
                       G_N_ELEMENTS(address_types));
    gtk_widget_set_vexpand((GtkWidget *) compose_window->recipients[1], TRUE);
    g_signal_connect_swapped(compose_window->recipient_view,
                             "view-changed",
                             G_CALLBACK(balsa_compose_window_set_title), compose_window);

    /* Subject: */
    create_string_entry(compose_window, grid, _("S_ubject:"), ++row,
                        compose_window->subject);
    g_signal_connect_swapped(G_OBJECT(compose_window->subject[1]), "changed",
                             G_CALLBACK(balsa_compose_window_set_title), compose_window);

    /* Reply To: */
    /* We already created it, so just increment row: */
    g_assert(++row == REPLY_TO_ROW);
#undef REPLY_TO_ROW

    /* fcc: mailbox folder where the message copy will be written to */
    if (balsa_app.fcc_mru == NULL)
        balsa_mblist_mru_add(&balsa_app.fcc_mru,
                             libbalsa_mailbox_get_url(balsa_app.sentbox));
    balsa_mblist_mru_add(&balsa_app.fcc_mru, "");
    if (balsa_app.copy_to_sentbox) {
        /* move the NULL option to the bottom */
        balsa_app.fcc_mru = g_list_reverse(balsa_app.fcc_mru);
        balsa_mblist_mru_add(&balsa_app.fcc_mru, "");
        balsa_app.fcc_mru = g_list_reverse(balsa_app.fcc_mru);
    }

    if (compose_window->draft_message != NULL) {
        LibBalsaMessageHeaders *headers;

        headers = libbalsa_message_get_headers(compose_window->draft_message);
        if (headers->fcc_url != NULL)
            balsa_mblist_mru_add(&balsa_app.fcc_mru, headers->fcc_url);
    }

    compose_window->fcc[1] =
        balsa_mblist_mru_option_menu(GTK_WINDOW(compose_window),
                                     &balsa_app.fcc_mru);
    create_email_or_string_entry(compose_window, grid, _("F_CC:"), ++row,
                                 compose_window->fcc);

    return grid;
}


static GtkWidget *
sw_attachment_list(BalsaComposeWindow *compose_window)
{
    GtkWidget *grid;
    GtkWidget *label;
    GtkWidget *sw;
    GtkListStore *store;
    GtkWidget *tree_view;
    GtkCellRenderer *renderer;
    GtkTreeView *view;
    GtkTreeViewColumn *column;
    GtkWidget *frame;
    GdkContentFormats *formats;
    GtkGesture *gesture;

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    g_object_set(G_OBJECT(grid), "margin", 6, NULL);

    /* Attachment list */
    label = gtk_label_new_with_mnemonic(_("_Attachments:"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    g_object_set(label, "margin", GNOME_PAD_SMALL, NULL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);

    store = gtk_list_store_new(ATTACH_NUM_COLUMNS,
                               BALSA_TYPE_ATTACH_INFO,
                               GDK_TYPE_PIXBUF,
                               G_TYPE_STRING,
                               G_TYPE_INT,
                               G_TYPE_UINT64,
                               G_TYPE_STRING);

    compose_window->tree_view = tree_view =
            gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_widget_set_vexpand(tree_view, TRUE);
    view = GTK_TREE_VIEW(tree_view);
    gtk_tree_view_set_headers_visible(view, TRUE);
    g_object_unref(store);

    /* column for type icon */
    renderer = gtk_cell_renderer_pixbuf_new();
    g_object_set(G_OBJECT(renderer), "xalign", 0.0, NULL);
    gtk_tree_view_insert_column_with_attributes(view,
                                                -1, NULL, renderer,
                                                "pixbuf", ATTACH_ICON_COLUMN,
                                                NULL);

    /* column for the mime type */
    renderer = gtk_cell_renderer_text_new();
    g_object_set(G_OBJECT(renderer), "xalign", 0.0, NULL);
    gtk_tree_view_insert_column_with_attributes(view,
                                                -1, _("Type"), renderer,
                                                "text", ATTACH_TYPE_COLUMN,
                                                NULL);

    /* column for the attachment mode */
    renderer = gtk_cell_renderer_text_new();
    g_object_set(G_OBJECT(renderer), "xalign", 0.0, NULL);
    column = gtk_tree_view_column_new_with_attributes(_("Mode"), renderer,
                                                      "text", ATTACH_MODE_COLUMN,
                                                      NULL);
    gtk_tree_view_column_set_cell_data_func(column,
                                            renderer, render_attach_mode,
                                            NULL, NULL);
    gtk_tree_view_append_column(view, column);

    /* column for the attachment size */
    renderer = gtk_cell_renderer_text_new();
    g_object_set(G_OBJECT(renderer), "xalign", 1.0, NULL);
    column = gtk_tree_view_column_new_with_attributes(_("Size"), renderer,
                                                      "text", ATTACH_SIZE_COLUMN,
                                                      NULL);
    gtk_tree_view_column_set_cell_data_func(column,
                                            renderer, render_attach_size,
                                            NULL, NULL);
    gtk_tree_view_append_column(view, column);

    /* column for the file type/description */
    renderer = gtk_cell_renderer_text_new();
    g_object_set(G_OBJECT(renderer), "xalign", 0.0, NULL);
    gtk_tree_view_insert_column_with_attributes(view,
                                                -1, _("Description"), renderer,
                                                "text", ATTACH_DESC_COLUMN,
                                                NULL);

    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(view),
                                GTK_SELECTION_SINGLE);

    gesture = gtk_gesture_multi_press_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 0);
    g_signal_connect(gesture, "pressed",
                     G_CALLBACK(sw_gesture_pressed_cb), NULL);
    gtk_widget_add_controller(tree_view, GTK_EVENT_CONTROLLER(gesture));

    g_signal_connect(view, "popup-menu",
                     G_CALLBACK(attachment_popup_cb), NULL);

    g_signal_connect(G_OBJECT(compose_window), "drag_data_received",
                     G_CALLBACK(attachments_add), compose_window);

    formats = gdk_content_formats_new(drop_types, G_N_ELEMENTS(drop_types));
    gtk_drag_dest_set(GTK_WIDGET(compose_window), GTK_DEST_DEFAULT_ALL,
                      formats,
                      GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
    gdk_content_formats_unref(formats);

    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), tree_view);
    gtk_container_add(GTK_CONTAINER(frame), sw);

    gtk_widget_set_hexpand(frame, TRUE);
    gtk_grid_attach(GTK_GRID(grid), frame, 1, 0, 1, 1);

    return grid;
}


typedef struct {
    gchar   *name;
    gboolean found;
} has_file_attached_t;

static gboolean
has_file_attached(GtkTreeModel *model,
                  GtkTreePath  *path,
                  GtkTreeIter  *iter,
                  gpointer      data)
{
    has_file_attached_t *find_file = (has_file_attached_t *)data;
    BalsaAttachInfo *info;
    const gchar *uri;

    gtk_tree_model_get(model, iter, ATTACH_INFO_COLUMN, &info, -1);
    if (!info)
        return FALSE;

    uri = libbalsa_vfs_get_uri(info->file_uri);
    if (g_strcmp0(find_file->name, uri) == 0)
        find_file->found = TRUE;
    g_object_unref(info);

    return find_file->found;
}


/* drag_data_quote - text area D&D callback */
static void
drag_data_quote(GtkWidget        *widget,
                GdkDragContext   *context,
                GtkSelectionData *selection_data,
                guint32           time,
                BalsaComposeWindow     *compose_window)
{
    const gchar *target;
    GtkTextBuffer *buffer;
    BalsaIndex *index;
    LibBalsaMailbox *mailbox;
    GArray *selected;
    guint i;

    target = gtk_selection_data_get_target(selection_data);

    if (target == g_intern_static_string(drop_types[TARGET_MESSAGES])) {
        index =
            *(BalsaIndex **) gtk_selection_data_get_data(selection_data);
        mailbox  = balsa_index_get_mailbox(index);
        selected = balsa_index_selected_msgnos_new(index);
        buffer   = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));

        for (i = 0; i < selected->len; i++) {
            guint msgno = g_array_index(selected, guint, i);
            LibBalsaMessage *message;
            GString *body;

            message = libbalsa_mailbox_get_message(mailbox, msgno);
            if (!message)
                continue;

            body = quote_message_body(compose_window, message, QUOTE_ALL);
            g_object_unref(message);
            gtk_text_buffer_insert_at_cursor(buffer, body->str, body->len);
            g_string_free(body, TRUE);
        }
        balsa_index_selected_msgnos_free(index, selected);
    } else if (target == g_intern_static_string(drop_types[TARGET_URI_LIST])) {
        GSList *uri_list, *list;

        uri_list = uri2gslist((gchar *) gtk_selection_data_get_data(selection_data));
        for (list = uri_list; list != NULL; list = list->next) {
            /* Since current GtkTextView gets this signal twice for
             * every action (#150141) we need to check for duplicates,
             * which is a good idea anyway. */
            has_file_attached_t find_file;

            find_file.name  = list->data;
            find_file.found = FALSE;
            if (compose_window->tree_view)
                gtk_tree_model_foreach(BALSA_MSG_ATTACH_MODEL(compose_window),
                                       has_file_attached, &find_file);
            if (!find_file.found)
                add_attachment(compose_window, list->data, FALSE, NULL);
            g_free(list->data);
        }
        g_slist_free(uri_list);
    }

    gtk_drag_finish(context, TRUE, time);
}


/* create_text_area
   Creates the text entry part of the compose window.
 */
#ifdef HAVE_GTKSOURCEVIEW

static void
sw_can_undo_cb(GtkSourceBuffer *source_buffer,
               GParamSpec      *arg1,
               BalsaComposeWindow    *compose_window)
{
    gboolean can_undo;

    g_object_get(G_OBJECT(source_buffer), "can-undo", &can_undo, NULL);
    sw_action_set_enabled(compose_window, "undo", can_undo);
}


static void
sw_can_redo_cb(GtkSourceBuffer *source_buffer,
               GParamSpec      *arg1,
               BalsaComposeWindow    *compose_window)
{
    gboolean can_redo;

    g_object_get(G_OBJECT(source_buffer), "can-redo", &can_redo, NULL);
    sw_action_set_enabled(compose_window, "redo", can_redo);
}


#endif                          /* HAVE_GTKSOURCEVIEW */

static GtkWidget *
create_text_area(BalsaComposeWindow *compose_window)
{
    GtkTextView *text_view;
    GtkTextBuffer *buffer;
#if HAVE_GSPELL
    GspellTextBuffer *gspell_buffer;
    GspellChecker *checker;
#   if HAVE_GSPELL_1_2
    GspellTextView *gspell_view;
#   endif                       /* HAVE_GSPELL_1_2 */
#endif                          /* HAVE_GSPELL */
    GtkWidget *scroll;
    GdkContentFormats *formats;

#if HAVE_GTKSOURCEVIEW
    compose_window->text = libbalsa_source_view_new(TRUE);
#else                           /* HAVE_GTKSOURCEVIEW */
    compose_window->text = gtk_text_view_new();
#endif                          /* HAVE_GTKSOURCEVIEW */
    text_view = GTK_TEXT_VIEW(compose_window->text);
    gtk_text_view_set_left_margin(text_view, 2);
    gtk_text_view_set_right_margin(text_view, 2);

    /* set the message font */
    if (!balsa_app.use_system_fonts) {
        gchar *css;
        GtkCssProvider *css_provider;

        css = libbalsa_font_string_to_css(balsa_app.message_font,
                                          BALSA_COMPOSE_ENTRY);

        css_provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css_provider, css, -1);
        g_free(css);

        gtk_widget_set_name((GtkWidget *) compose_window->text, BALSA_COMPOSE_ENTRY);
        gtk_style_context_add_provider(gtk_widget_get_style_context((GtkWidget *) compose_window->text),
                                       GTK_STYLE_PROVIDER(css_provider),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css_provider);
    }

    buffer = gtk_text_view_get_buffer(text_view);
#ifdef HAVE_GTKSOURCEVIEW
    g_signal_connect(G_OBJECT(buffer), "notify::can-undo",
                     G_CALLBACK(sw_can_undo_cb), compose_window);
    g_signal_connect(G_OBJECT(buffer), "notify::can-redo",
                     G_CALLBACK(sw_can_redo_cb), compose_window);
#else                           /* HAVE_GTKSOURCEVIEW */
    compose_window->buffer2 =
        gtk_text_buffer_new(gtk_text_buffer_get_tag_table(buffer));
#endif                          /* HAVE_GTKSOURCEVIEW */
    gtk_text_buffer_create_tag(buffer, "url", NULL, NULL);
    gtk_text_view_set_editable(text_view, TRUE);
    gtk_text_view_set_wrap_mode(text_view, GTK_WRAP_WORD_CHAR);

#if HAVE_GSPELL
    if (sw_action_get_enabled(compose_window, "spell-check")) {
        gspell_buffer = gspell_text_buffer_get_from_gtk_text_buffer(buffer);
        checker       = gspell_checker_new(NULL);
        gspell_text_buffer_set_spell_checker(gspell_buffer, checker);
        g_object_unref(checker);

#   if HAVE_GSPELL_1_2
        gspell_view = gspell_text_view_get_from_gtk_text_view(text_view);
        gspell_text_view_set_enable_language_menu(gspell_view, TRUE);
#   endif                       /* HAVE_GSPELL_1_2 */
    }
#endif                          /* HAVE_GSPELL */

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_container_add(GTK_CONTAINER(scroll), compose_window->text);
    g_signal_connect(G_OBJECT(compose_window->text), "drag_data_received",
                     G_CALLBACK(drag_data_quote), compose_window);

    formats = gdk_content_formats_new(drop_types, G_N_ELEMENTS(drop_types));
    /* GTK_DEST_DEFAULT_ALL in drag_set would trigger bug 150141 */
    gtk_drag_dest_set(GTK_WIDGET(compose_window->text), 0,
                      formats,
                      GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
    gdk_content_formats_unref(formats);

    return scroll;
}


/* Check whether the string can be converted. */
static gboolean
sw_can_convert(const gchar *string,
               gssize       len,
               const gchar *to_codeset,
               const gchar *from_codeset,
               gchar      **result)
{
    gsize bytes_read, bytes_written;
    GError *err = NULL;
    gchar *s;

    if (!(to_codeset && from_codeset))
        return FALSE;

    s = g_convert(string, len, to_codeset, from_codeset,
                  &bytes_read, &bytes_written, &err);
    if (err) {
        g_error_free(err);
        g_free(s);
        s = NULL;
    }

    if (result)
        *result = s;
    else
        g_free(s);

    return !err;
}


/* continue_body --------------------------------------------------------
   a short-circuit procedure for the 'Continue action'
   basically copies the first text/plain part over to the entry field.
   Attachments (if any) are saved temporarily in subfolders to preserve
   their original names and then attached again.
   NOTE that rbdy == NULL if message has no text parts.
 */
static void
continue_body(BalsaComposeWindow    *compose_window,
              LibBalsaMessage *message)
{
    LibBalsaMessageBody *body;

    body = libbalsa_message_get_body_list(message);
    if (body) {
        if (libbalsa_message_body_type(body) == LIBBALSA_MESSAGE_BODY_TYPE_MULTIPART)
            body = body->parts;
        /* if the first part is of type text/plain with a NULL filename, it
           was the message... */
        if (body && !body->filename) {
            GString *rbdy;
            gchar *body_type      = libbalsa_message_body_get_mime_type(body);
            gint llen             = -1;
            GtkTextBuffer *buffer =
                gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));

            if (compose_window->flow && libbalsa_message_body_is_flowed(body))
                llen = balsa_app.wraplength;
            if (!strcmp(body_type, "text/plain") &&
                (rbdy = process_mime_part(message, body, NULL, llen, FALSE,
                                          compose_window->flow))) {
                gtk_text_buffer_insert_at_cursor(buffer, rbdy->str, rbdy->len);
                g_string_free(rbdy, TRUE);
            }
            g_free(body_type);
            body = body->next;
        }
        while (body) {
            gchar *name, *body_type, *tmp_file_name;
            GError *err  = NULL;
            gboolean res = FALSE;

            if (body->filename) {
                libbalsa_mktempdir(&tmp_file_name);
                name = g_strdup_printf("%s/%s", tmp_file_name, body->filename);
                g_free(tmp_file_name);
                res = libbalsa_message_body_save(body, name,
                                                 LIBBALSA_MESSAGE_BODY_SAFE,
                                                 FALSE, &err);
            } else {
                int fd;

                if ((fd = g_file_open_tmp("balsa-continue-XXXXXX", &name, NULL)) > 0) {
                    GMimeStream *tmp_stream;

                    if ((tmp_stream = g_mime_stream_fs_new(fd)) != NULL)
                        res = libbalsa_message_body_save_stream(body, tmp_stream, FALSE, &err);
                    else
                        close(fd);
                }
            }
            if (!res) {
                balsa_information_parented(GTK_WINDOW(compose_window),
                                           LIBBALSA_INFORMATION_ERROR,
                                           _("Could not save attachment: %s"),
                                           err ? err->message : "Unknown error");
                g_clear_error(&err);
                /* FIXME: do not try any further? */
            }
            body_type     = libbalsa_message_body_get_mime_type(body);
            tmp_file_name = g_filename_to_uri(name, NULL, NULL);
            g_free(name);
            add_attachment(compose_window, tmp_file_name, TRUE, body_type);
            g_free(body_type);
            g_free(tmp_file_name);
            body = body->next;
        }
    }
}


static gchar *
message_part_get_subject(LibBalsaMessageBody *part)
{
    const gchar *subj = NULL;
    gchar *subject;

    if (part->embhdrs != NULL)
        subj = part->embhdrs->subject;
    if (subj == NULL) {
        if (part->message != NULL)
            subj = libbalsa_message_get_subject(part->message);
        if (subj == NULL)
            subj = _("No subject");
    }

    subject = g_strdup(subj);
    libbalsa_utf8_sanitize(&subject, balsa_app.convert_unknown_8bit, NULL);

    return subject;
}


/* --- stuff for collecting parts for a reply --- */

enum {
    QUOTE_INCLUDE,
    QUOTE_DESCRIPTION,
    QUOTE_BODY,
    QOUTE_NUM_ELEMS
};

static void
tree_add_quote_body(LibBalsaMessageBody *body,
                    GtkTreeStore        *store,
                    GtkTreeIter         *parent)
{
    GtkTreeIter iter;
    gchar *mime_type = libbalsa_message_body_get_mime_type(body);
    const gchar *disp_type;
    static gboolean preselect;
    gchar *description;

    gtk_tree_store_append(store, &iter, parent);
    if (body->mime_part)
        disp_type = g_mime_object_get_disposition(body->mime_part);
    else
        disp_type = NULL;
    /* cppcheck-suppress nullPointer */
    preselect = !disp_type || *disp_type == '\0' ||
        !g_ascii_strcasecmp(disp_type, "inline");
    if (body->filename && *body->filename) {
        if (preselect)
            description = g_strdup_printf(_("inlined file “%s” (%s)"),
                                          body->filename, mime_type);
        else
            description = g_strdup_printf(_("attached file “%s” (%s)"),
                                          body->filename, mime_type);
    } else {
        if (preselect)
            description = g_strdup_printf(_("inlined %s part"), mime_type);
        else
            description = g_strdup_printf(_("attached %s part"), mime_type);
    }
    g_free(mime_type);
    gtk_tree_store_set(store, &iter,
                       QUOTE_INCLUDE, preselect,
                       QUOTE_DESCRIPTION, description,
                       QUOTE_BODY, body,
                       -1);
    g_free(description);
}


static gint
scan_bodies(GtkTreeStore        *bodies,
            GtkTreeIter         *parent,
            LibBalsaMessageBody *body,
            gboolean             ignore_html,
            gboolean             container_mp_alt)
{
    gchar *mime_type;
    gint count = 0;

    while (body) {
        switch (libbalsa_message_body_type(body)) {
        case LIBBALSA_MESSAGE_BODY_TYPE_TEXT:
        {
            LibBalsaHTMLType html_type;

            mime_type = libbalsa_message_body_get_mime_type(body);
            html_type = libbalsa_html_type(mime_type);
            g_free(mime_type);

            /* On a multipart/alternative, ignore_html defines if html or
             * non-html parts will be added. Eject from the container when
             * the first part has been found.
             * Otherwise, select all text parts. */
            if (container_mp_alt) {
                if ((ignore_html && (html_type == LIBBALSA_HTML_TYPE_NONE)) ||
                    (!ignore_html && (html_type != LIBBALSA_HTML_TYPE_NONE))) {
                    tree_add_quote_body(body, bodies, parent);
                    return count + 1;
                }
            } else {
                tree_add_quote_body(body, bodies, parent);
                count++;
            }
            break;
        }

        case LIBBALSA_MESSAGE_BODY_TYPE_MULTIPART:
            mime_type = libbalsa_message_body_get_mime_type(body);
            count    += scan_bodies(bodies, parent, body->parts, ignore_html,
                                    !g_ascii_strcasecmp(mime_type, "multipart/alternative"));
            g_free(mime_type);
            break;

        case LIBBALSA_MESSAGE_BODY_TYPE_MESSAGE:
        {
            GtkTreeIter iter;
            gchar *description = NULL;

            mime_type = libbalsa_message_body_get_mime_type(body);
            if ((g_ascii_strcasecmp(mime_type, "message/rfc822") == 0) &&
                body->embhdrs) {
                gchar *from = balsa_message_sender_to_gchar(body->embhdrs->from, 0);
                gchar *subj = g_strdup(body->embhdrs->subject);


                libbalsa_utf8_sanitize(&from, balsa_app.convert_unknown_8bit, NULL);
                libbalsa_utf8_sanitize(&subj, balsa_app.convert_unknown_8bit, NULL);
                description =
                    g_strdup_printf(_("message from %s, subject “%s”"),
                                    from, subj);
                g_free(from);
                g_free(subj);
            } else {
                description = g_strdup(mime_type);
            }

            gtk_tree_store_append(bodies, &iter, parent);
            gtk_tree_store_set(bodies, &iter,
                               QUOTE_INCLUDE, FALSE,
                               QUOTE_DESCRIPTION, description,
                               QUOTE_BODY, NULL,
                               -1);
            g_free(mime_type);
            g_free(description);
            count += scan_bodies(bodies, &iter, body->parts, ignore_html, 0);
        }

        default:
            break;
        }

        body = body->next;
    }

    return count;
}


static void
set_all_cells(GtkTreeModel  *model,
              GtkTreeIter   *iter,
              const gboolean value)
{
    do {
        GtkTreeIter children;

        if (gtk_tree_model_iter_children(model, &children, iter))
            set_all_cells(model, &children, value);
        gtk_tree_store_set(GTK_TREE_STORE(model), iter, QUOTE_INCLUDE, value, -1);
    } while (gtk_tree_model_iter_next(model, iter));
}


static gboolean
calculate_expander_toggles(GtkTreeModel *model,
                           GtkTreeIter  *iter)
{
    gint count, on;

    count = on = 0;
    do {
        GtkTreeIter children;
        gboolean value;

        if (gtk_tree_model_iter_children(model, &children, iter)) {
            value = calculate_expander_toggles(model, &children);
            gtk_tree_store_set(GTK_TREE_STORE(model), iter, QUOTE_INCLUDE, value, -1);
        } else {
            gtk_tree_model_get(model, iter, QUOTE_INCLUDE, &value, -1);
        }
        if (value)
            on++;
        count++;
    } while (gtk_tree_model_iter_next(model, iter));

    return count == on;
}


static void
cell_toggled_cb(GtkCellRendererToggle *cell,
                gchar                 *path_str,
                GtkTreeView           *treeview)
{
    GtkTreeModel *model = NULL;
    GtkTreePath *path;
    GtkTreeIter iter;
    GtkTreeIter children;
    gboolean active;

    g_return_if_fail (GTK_IS_TREE_VIEW (treeview));
    if (!(model = gtk_tree_view_get_model(treeview)))
        return;

    path = gtk_tree_path_new_from_string(path_str);
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return;

    gtk_tree_path_free(path);

    gtk_tree_model_get(model, &iter,
                       QUOTE_INCLUDE, &active,
                       -1);
    gtk_tree_store_set(GTK_TREE_STORE (model), &iter,
                       QUOTE_INCLUDE, !active,
                       -1);
    if (gtk_tree_model_iter_children(model, &children, &iter))
        set_all_cells(model, &children, !active);
    gtk_tree_model_get_iter_first(model, &children);
    calculate_expander_toggles(model, &children);
}


static void
append_parts(GString         *q_body,
             LibBalsaMessage *message,
             GtkTreeModel    *model,
             GtkTreeIter     *iter,
             const gchar     *from_msg,
             gchar           *reply_prefix_str,
             gint             llen,
             gboolean         flow)
{
    gboolean used_from_msg = FALSE;

    do {
        GtkTreeIter children;

        if (gtk_tree_model_iter_children(model, &children, iter)) {
            gchar *description;

            gtk_tree_model_get(model, iter, QUOTE_DESCRIPTION, &description, -1);
            append_parts(q_body, message, model, &children, description,
                         reply_prefix_str, llen, flow);
            g_free(description);
        } else {
            gboolean do_include;

            gtk_tree_model_get(model, iter, QUOTE_INCLUDE, &do_include, -1);
            if (do_include) {
                LibBalsaMessageBody *this_body;

                gtk_tree_model_get(model, iter, QUOTE_BODY, &this_body, -1);
                if (this_body) {
                    GString *this_part;
                    this_part = process_mime_part(message, this_body,
                                                  reply_prefix_str, llen,
                                                  FALSE, flow);

                    if ((q_body->len > 0) && (q_body->str[q_body->len - 1] != '\n'))
                        g_string_append_c(q_body, '\n');
                    if (!used_from_msg && from_msg) {
                        g_string_append_printf(q_body, "\n======%s %s======\n", _(
                                                   "quoted"), from_msg);
                        used_from_msg = TRUE;
                    } else if (q_body->len > 0) {
                        if (this_body->filename)
                            g_string_append_printf(q_body, "\n------%s “%s”------\n",
                                                   _("quoted attachment"), this_body->filename);






                        else
                            g_string_append_printf(q_body, "\n------%s------\n",
                                                   _("quoted attachment"));
                    }
                    g_string_append(q_body, this_part->str);
                    g_string_free(this_part, TRUE);
                }
            }
        }
    } while (gtk_tree_model_iter_next(model, iter));
}


static gboolean
quote_parts_select_dlg(GtkTreeStore *tree_store,
                       GtkWindow    *parent)
{
    GtkWidget *dialog;
    GtkWidget *label;
    GtkWidget *image;
    GtkWidget *hbox;
    GtkWidget *vbox;
    GtkWidget *scroll;
    GtkWidget *tree_view;
    GtkTreeViewColumn *column;
    GtkCellRenderer *renderer;
    GtkTreeIter iter;
    gboolean result;
    GtkBox *content_box;

    dialog = gtk_dialog_new_with_buttons(_("Select parts for quotation"),
                                         parent,
                                         GTK_DIALOG_DESTROY_WITH_PARENT |
                                         libbalsa_dialog_flags(),
                                         _("_OK"), GTK_RESPONSE_OK,
                                         _("_Cancel"), GTK_RESPONSE_CANCEL,
                                         NULL);
#if HAVE_MACOSX_DESKTOP
    libbalsa_macosx_menu_for_parent(dialog, parent);
#endif

    label = gtk_label_new(_("Select the parts of the message"
                            " which shall be quoted in the reply"));
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_valign(label, GTK_ALIGN_START);

    image = gtk_image_new_from_icon_name("dialog-question");
    gtk_widget_set_valign(image, GTK_ALIGN_START);

    /* stolen form gtk/gtkmessagedialog.c */
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    gtk_box_pack_start(GTK_BOX(vbox), label);
    gtk_box_pack_start(GTK_BOX(hbox), image);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), vbox);

    content_box = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
    gtk_widget_set_vexpand(hbox, TRUE);
    gtk_box_pack_start(content_box, hbox);

    g_object_set(G_OBJECT(hbox), "margin", 5, NULL);
    gtk_box_set_spacing(content_box, 14);

    /* scrolled window for the tree view */
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    g_object_set(G_OBJECT(scroll), "margin", 5, NULL);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), scroll);

    /* add the tree view */
    tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(tree_store));
    gtk_widget_set_size_request(tree_view, -1, 100);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree_view), FALSE);
    renderer = gtk_cell_renderer_toggle_new();
    g_signal_connect(renderer, "toggled", G_CALLBACK(cell_toggled_cb),
                     tree_view);
    column = gtk_tree_view_column_new_with_attributes(NULL, renderer,
                                                      "active", QUOTE_INCLUDE,
                                                      NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);
    gtk_tree_view_set_expander_column(GTK_TREE_VIEW(tree_view), column);
    column = gtk_tree_view_column_new_with_attributes(NULL, gtk_cell_renderer_text_new(),
                                                      "text", QUOTE_DESCRIPTION,
                                                      NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(tree_view));
    gtk_tree_model_get_iter_first(GTK_TREE_MODEL(tree_store), &iter);
    calculate_expander_toggles(GTK_TREE_MODEL(tree_store), &iter);

    /* add, show & run */
    gtk_container_add(GTK_CONTAINER(scroll), tree_view);
    result = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
    gtk_widget_destroy(dialog);
    return result;
}


static gboolean
tree_find_single_part(GtkTreeModel *model,
                      GtkTreePath  *path,
                      GtkTreeIter  *iter,
                      gpointer      data)
{
    LibBalsaMessageBody **this_body = (LibBalsaMessageBody **) data;

    gtk_tree_model_get(model, iter, QUOTE_BODY, this_body, -1);
    if (*this_body)
        return TRUE;
    else
        return FALSE;
}


static GString *
collect_for_quote(BalsaComposeWindow        *compose_window,
                  LibBalsaMessageBody *root,
                  gchar               *reply_prefix_str,
                  gint                 llen,
                  gboolean             ignore_html,
                  gboolean             flow)
{
    GtkTreeStore *tree_store;
    gint text_bodies;
    LibBalsaMessage *message;
    GString *q_body = NULL;


    if (!root)
        return q_body;

    message = root->message;
    libbalsa_message_body_ref(message, FALSE, FALSE);

    /* scan the message and collect text parts which might be included
     * in the reply, and if there is only one return this part */
    tree_store = gtk_tree_store_new(QOUTE_NUM_ELEMS,
                                    G_TYPE_BOOLEAN, G_TYPE_STRING,
                                    G_TYPE_POINTER);
    text_bodies = scan_bodies(tree_store, NULL, root, ignore_html, FALSE);
    if (text_bodies == 1) {
        /* note: the only text body may be buried in an attached message, so
         * we have to search the tree store... */
        LibBalsaMessageBody *this_body;

        gtk_tree_model_foreach(GTK_TREE_MODEL(tree_store), tree_find_single_part,
                               &this_body);
        if (this_body)
            q_body = process_mime_part(message, this_body, reply_prefix_str,
                                       llen, FALSE, flow);
    } else if (text_bodies > 1) {
        if (quote_parts_select_dlg(tree_store, GTK_WINDOW(compose_window))) {
            GtkTreeIter iter;

            q_body = g_string_new("");
            gtk_tree_model_get_iter_first(GTK_TREE_MODEL(tree_store), &iter);
            append_parts(q_body, message, GTK_TREE_MODEL(tree_store), &iter, NULL,
                         reply_prefix_str, llen, flow);
        }
    }

    /* clean up */
    g_object_unref(G_OBJECT(tree_store));
    libbalsa_message_body_unref(message);
    return q_body;
}


/* quote_body -----------------------------------------------------------
   quotes properly the body of the message.
   Use GString to optimize memory usage.
   Specifying type explicitly allows for later message quoting when
   eg. a new message is composed.
 */
static GString *
quote_body(BalsaComposeWindow           *compose_window,
           LibBalsaMessageHeaders *headers,
           const gchar            *message_id,
           GList                  *references,
           LibBalsaMessageBody    *root,
           QuoteType               qtype)
{
    GString *body;
    gchar *str, *date = NULL;
    gchar *personStr;
    const gchar *orig_address;

    g_return_val_if_fail(headers, NULL);

    if (headers->from &&
        (orig_address =
             libbalsa_address_get_name_from_list(headers->from))) {
        personStr = g_strdup(orig_address);
        libbalsa_utf8_sanitize(&personStr,
                               balsa_app.convert_unknown_8bit,
                               NULL);
    } else {
        personStr = g_strdup(_("you"));
    }

    if (headers->date)
        date = libbalsa_message_headers_date_to_utf8(headers,
                                                     balsa_app.date_string);

    if (qtype == QUOTE_HEADERS) {
        gchar *subject;

        str = g_strdup_printf(_("------forwarded message from %s------\n"),
                              personStr);
        body = g_string_new(str);
        g_free(str);

        if (date)
            g_string_append_printf(body, "%s %s\n", _("Date:"), date);

        subject = message_part_get_subject(root);
        if (subject)
            g_string_append_printf(body, "%s %s\n", _("Subject:"), subject);
        g_free(subject);

        if (headers->from) {
            gchar *from =
                internet_address_list_to_string(headers->from,
                                                FALSE);
            g_string_append_printf(body, "%s %s\n", _("From:"), from);
            g_free(from);
        }

        if (internet_address_list_length(headers->to_list) > 0) {
            gchar *to_list =
                internet_address_list_to_string(headers->to_list,
                                                FALSE);
            g_string_append_printf(body, "%s %s\n", _("To:"), to_list);
            g_free(to_list);
        }

        if (internet_address_list_length(headers->cc_list) > 0) {
            gchar *cc_list =
                internet_address_list_to_string(headers->cc_list,
                                                FALSE);
            g_string_append_printf(body, "%s %s\n", _("CC:"), cc_list);
            g_free(cc_list);
        }

        g_string_append_printf(body, _("Message-ID: %s\n"),
                               message_id);

        if (references) {
            GList *ref_list;

            g_string_append(body, _("References:"));

            for (ref_list = references; ref_list != NULL;
                 ref_list = ref_list->next) {
                g_string_append_printf(body, " <%s>",
                                       (gchar *) ref_list->data);
            }

            g_string_append_c(body, '\n');
        }
    } else {
        if (date)
            str = g_strdup_printf(_("On %s, %s wrote:\n"), date, personStr);
        else
            str = g_strdup_printf(_("%s wrote:\n"), personStr);

        /* scan the message and collect text parts which might be included
         * in the reply */
        body = collect_for_quote(compose_window, root,
                                 qtype == QUOTE_ALL ? balsa_app.quote_str : NULL,
                                 compose_window->flow ? -1 : balsa_app.wraplength,
                                 balsa_app.reply_strip_html, compose_window->flow);
        if (body) {
            gchar *buf;

            buf = g_string_free(body, FALSE);
            libbalsa_utf8_sanitize(&buf, balsa_app.convert_unknown_8bit,
                                   NULL);
            body = g_string_new(buf);
            g_free(buf);
            g_string_prepend(body, str);
        } else {
            body = g_string_new(str);
        }
        g_free(str);
    }

    g_free(date);
    g_free(personStr);

    return body;
}


/* fill_body -------------------------------------------------------------
   fills the body of the message to be composed based on the given message.
   First quotes the original one, if autoquote is set,
   and then adds the signature.
   Optionally prepends the signature to quoted text.
 */
static void
fill_body_from_part(BalsaComposeWindow           *compose_window,
                    LibBalsaMessageHeaders *headers,
                    const gchar            *message_id,
                    GList                  *references,
                    LibBalsaMessageBody    *root,
                    QuoteType               qtype)
{
    GString *body;
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
    GtkTextIter start;

    g_assert(headers);

    body = quote_body(compose_window, headers, message_id, references,
                      root, qtype);

    g_return_if_fail(body != NULL);

    if (body->len && (body->str[body->len] != '\n'))
        g_string_append_c(body, '\n');
    gtk_text_buffer_insert_at_cursor(buffer, body->str, body->len);

    if (qtype == QUOTE_HEADERS)
        gtk_text_buffer_get_end_iter(buffer, &start);
    else
        gtk_text_buffer_get_start_iter(buffer, &start);

    gtk_text_buffer_place_cursor(buffer, &start);
    g_string_free(body, TRUE);
}


static GString *
quote_message_body(BalsaComposeWindow    *compose_window,
                   LibBalsaMessage *message,
                   QuoteType        qtype)
{
    GString *res;
    if (libbalsa_message_body_ref(message, FALSE, FALSE)) {
        res = quote_body(compose_window,
                         libbalsa_message_get_headers(message),
                         libbalsa_message_get_message_id(message),
                         libbalsa_message_get_references(message),
                         libbalsa_message_get_body_list(message),
                         qtype);
        libbalsa_message_body_unref(message);
    } else {
        res = g_string_new("");
    }
    return res;
}


static void
fill_body_from_message(BalsaComposeWindow    *compose_window,
                       LibBalsaMessage *message,
                       QuoteType        qtype)
{
    fill_body_from_part(compose_window,
                        libbalsa_message_get_headers(message),
                        libbalsa_message_get_message_id(message),
                        libbalsa_message_get_references(message),
                        libbalsa_message_get_body_list(message),
                        qtype);
}


static void
sw_insert_sig_activated(GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       data)
{
    BalsaComposeWindow *compose_window = data;
    gchar *signature;
    GError *error = NULL;

    signature = libbalsa_identity_get_signature(compose_window->ident, &error);

    if (signature != NULL) {
        GtkTextBuffer *buffer =
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
#if !HAVE_GTKSOURCEVIEW
        sw_buffer_save(compose_window);
#endif                          /* HAVE_GTKSOURCEVIEW */
        sw_buffer_signals_block(compose_window, buffer);
        gtk_text_buffer_insert_at_cursor(buffer, signature, -1);
        sw_buffer_signals_unblock(compose_window, buffer);

        g_free(signature);
    } else if (error != NULL) {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_ERROR,
                                   error->message);
        g_error_free(error);
    }
}


static void
sw_quote_activated(GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    insert_selected_messages(compose_window, QUOTE_ALL);
}


/** Generates a new subject for forwarded messages based on a message
    being responded to and identity.
 */
static char *
generate_forwarded_subject(const char             *orig_subject,
                           LibBalsaMessageHeaders *headers,
                           LibBalsaIdentity       *ident)
{
    char *newsubject;
    const gchar *forward_string = libbalsa_identity_get_forward_string(ident);

    if (!orig_subject) {
        if (headers && headers->from) {
            newsubject = g_strdup_printf("%s from %s",
                                         forward_string,
                                         libbalsa_address_get_mailbox_from_list
                                             (headers->from));
        } else {
            newsubject = g_strdup(forward_string);
        }
    } else {
        const char *tmp = orig_subject;
        if (g_ascii_strncasecmp(tmp, "fwd:", 4) == 0) {
            tmp += 4;
        } else if (g_ascii_strncasecmp(tmp, _("Fwd:"),
                                       strlen(_("Fwd:"))) == 0) {
            tmp += strlen(_("Fwd:"));
        } else {
            size_t i = strlen(forward_string);
            if (g_ascii_strncasecmp(tmp, forward_string, i) == 0)
                tmp += i;
        }
        while ( *tmp && isspace((int)*tmp)) {
            tmp++;
        }
        if (headers && headers->from) {
            newsubject =
                g_strdup_printf("%s %s [%s]",
                                forward_string,
                                tmp,
                                libbalsa_address_get_mailbox_from_list
                                    (headers->from));
        } else {
            newsubject =
                g_strdup_printf("%s %s",
                                forward_string,
                                tmp);
            g_strchomp(newsubject);
        }
    }
    return newsubject;
}


/* compose_window_set_subject_from_body:
   set subject entry based on given replied/forwarded/continued message
   and the compose type.
 */
static void
compose_window_set_subject_from_body(BalsaComposeWindow        *compose_window,
                            LibBalsaMessageBody *part,
                            LibBalsaIdentity    *ident)
{
    const gchar *reply_string = libbalsa_identity_get_reply_string(ident);
    gchar *subject;

    if (!part)
        return;

    subject = message_part_get_subject(part);

    if (!compose_window->is_continue) {
        gchar *newsubject = NULL;
        const gchar *tmp;
        LibBalsaMessageHeaders *headers;

        switch (compose_window->type) {
        case SEND_REPLY:
        case SEND_REPLY_ALL:
        case SEND_REPLY_GROUP:
            if (!subject) {
                subject = g_strdup(reply_string);
                break;
            }

            tmp = subject;
            if ((g_ascii_strncasecmp(tmp, "re:", 3) == 0) ||
                (g_ascii_strncasecmp(tmp, "aw:", 3) == 0)) {
                tmp += 3;
            } else if (g_ascii_strncasecmp(tmp, _("Re:"), strlen(_("Re:")))
                       == 0) {
                tmp += strlen(_("Re:"));
            } else {
                gint len = strlen(reply_string);
                if (g_ascii_strncasecmp(tmp, reply_string, len) == 0)
                    tmp += len;
            }
            while (*tmp && isspace((int) *tmp)) {
                tmp++;
            }
            newsubject = g_strdup_printf("%s %s", reply_string, tmp);
            g_strchomp(newsubject);
            g_strdelimit(newsubject, "\r\n", ' ');
            break;

        case SEND_FORWARD_ATTACH:
        case SEND_FORWARD_INLINE:
            headers =
                part->embhdrs != NULL ? part->embhdrs :
                libbalsa_message_get_headers(part->message);
            newsubject =
                generate_forwarded_subject(subject, headers, ident);
            break;

        default:
            break;
        }

        if (newsubject) {
            g_free(subject);
            subject = newsubject;
        }
    }

    gtk_entry_set_text(GTK_ENTRY(compose_window->subject[1]), subject);
    g_free(subject);
}


static gboolean
sw_save_draft(BalsaComposeWindow *compose_window)
{
    GError *err = NULL;

    if (!message_postpone(compose_window)) {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_MESSAGE,
                                   _("Could not save message."));
        return FALSE;
    }

    if (!libbalsa_mailbox_open(balsa_app.draftbox, &err)) {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_WARNING,
                                   _("Could not open draftbox: %s"),
                                   err ? err->message : _("Unknown error"));
        g_clear_error(&err);
        return FALSE;
    }

    if (compose_window->draft_message != NULL) {
        LibBalsaMailbox *mailbox;

        g_object_set_data(G_OBJECT(compose_window->draft_message),
                          BALSA_SENDMSG_WINDOW_KEY, NULL);
        mailbox = libbalsa_message_get_mailbox(compose_window->draft_message);
        if (mailbox != NULL) {
            libbalsa_mailbox_close(mailbox,
                                   /* Respect pref setting: */
                                   balsa_app.expunge_on_close);
        }
        g_object_unref(compose_window->draft_message);
    }
    compose_window->state = SENDMSG_STATE_CLEAN;

    compose_window->draft_message =
        libbalsa_mailbox_get_message(balsa_app.draftbox,
                                     libbalsa_mailbox_total_messages
                                         (balsa_app.draftbox));
    g_object_set_data(G_OBJECT(compose_window->draft_message),
                      BALSA_SENDMSG_WINDOW_KEY, compose_window);
    balsa_information_parented(GTK_WINDOW(compose_window),
                               LIBBALSA_INFORMATION_MESSAGE,
                               _("Message saved."));

    return TRUE;
}


static gboolean
sw_autosave_timeout_cb(BalsaComposeWindow *compose_window)
{
    if (compose_window->state == SENDMSG_STATE_MODIFIED) {
        if (sw_save_draft(compose_window))
            compose_window->state = SENDMSG_STATE_AUTO_SAVED;
    }

    return TRUE;                /* do repeat it */
}


static void
setup_headers_from_message(BalsaComposeWindow    *compose_window,
                           LibBalsaMessage *message)
{
    LibBalsaMessageHeaders *headers;

    headers = libbalsa_message_get_headers(message);
    g_return_if_fail(headers != NULL);

    /* Try to make the blank line in the address view useful;
     * - never make it a Bcc: line;
     * - if Cc: is non-empty, make it a Cc: line;
     * - if Cc: is empty, make it a To: line
     * Note that if set-from-list is given an empty list, the blank line
     * will be a To: line */
    libbalsa_address_view_set_from_list(compose_window->recipient_view,
                                        "BCC:",
                                        headers->bcc_list);
    libbalsa_address_view_set_from_list(compose_window->recipient_view,
                                        "To:",
                                        headers->to_list);
    libbalsa_address_view_set_from_list(compose_window->recipient_view,
                                        "CC:",
                                        headers->cc_list);
}


/*
 * set_identity_from_mailbox
 *
 * Attempt to determine the default identity from the mailbox containing
 * the message.
 **/
static gboolean
set_identity_from_mailbox(BalsaComposeWindow    *compose_window,
                          LibBalsaMessage *message)
{
    if ((message != NULL) && (balsa_app.identities != NULL)) {
        LibBalsaMailbox *mailbox;

        mailbox = libbalsa_message_get_mailbox(message);
        if (mailbox != NULL) {
            const gchar *identity;
            GList *list;

            identity = libbalsa_mailbox_get_identity_name(mailbox);
            if (identity == NULL)
                return FALSE;

            for (list = balsa_app.identities; list != NULL; list = list->next) {
                LibBalsaIdentity *ident = LIBBALSA_IDENTITY(list->data);

                if (g_ascii_strcasecmp(libbalsa_identity_get_identity_name(ident),
                                       identity) == 0) {
                    compose_window->ident = ident;
                    return TRUE;
                }
            }
        }
    }

    return FALSE;               /* use default */
}


/*
 * guess_identity
 *
 * Attempt to determine if a message should be associated with a
 * particular identity, other than the default.  The to_list of the
 * original message needs to be set in order for it to work.
 **/
/* First a helper; groups cannot be nested, and are not allowed in the
 * From: list. */
/* Update: RFC 6854 allows groups in "From:" and "Sender:" */
static gboolean
guess_identity_from_list(BalsaComposeWindow        *compose_window,
                         InternetAddressList *list,
                         gboolean             allow_group)
{
    gint i;

    if (!list)
        return FALSE;

    allow_group = TRUE;
    for (i = 0; i < internet_address_list_length(list); i++) {
        InternetAddress *ia = internet_address_list_get_address(list, i);

        if (INTERNET_ADDRESS_IS_GROUP(ia)) {
            InternetAddressList *members =
                INTERNET_ADDRESS_GROUP(ia)->members;
            if (allow_group
                && guess_identity_from_list(compose_window, members, FALSE))
                return TRUE;
        } else {
            GList *l;

            for (l = balsa_app.identities; l; l = l->next) {
                LibBalsaIdentity *ident = LIBBALSA_IDENTITY(l->data);
                if (libbalsa_ia_rfc2821_equal(libbalsa_identity_get_address(ident),
                                              ia)) {
                    compose_window->ident = ident;
                    return TRUE;
                }
            }
        }
    }

    return FALSE;
}


static gboolean
guess_identity(BalsaComposeWindow    *compose_window,
               LibBalsaMessage *message)
{
    LibBalsaMessageHeaders *headers;

    if ((message == NULL) || (balsa_app.identities == NULL))
        return FALSE; /* use default */

    headers = libbalsa_message_get_headers(message);
    if (headers == NULL)
        return FALSE;

    if (compose_window->is_continue)
        return guess_identity_from_list(compose_window, headers->from, FALSE);

    if (compose_window->type != SEND_NORMAL) {
        /* compose_window->type == SEND_REPLY || compose_window->type == SEND_REPLY_ALL ||
         *  compose_window->type == SEND_REPLY_GROUP || compose_window->type == SEND_FORWARD_ATTACH ||
         *  compose_window->type == SEND_FORWARD_INLINE */
        return guess_identity_from_list(compose_window, headers->to_list, TRUE)
               || guess_identity_from_list(compose_window, headers->cc_list, TRUE);
    }

    return FALSE;
}


static void
setup_headers_from_identity(BalsaComposeWindow     *compose_window,
                            LibBalsaIdentity *ident)
{
    gtk_combo_box_set_active(GTK_COMBO_BOX(compose_window->from[1]),
                             g_list_index(balsa_app.identities, ident));

    /* Make sure the blank line is "To:" */
    libbalsa_address_view_add_from_string(compose_window->recipient_view,
                                          "To:", NULL);
}


static int
comp_send_locales(const void *a,
                  const void *b)
{
    return g_utf8_collate(((struct SendLocales *)a)->lang_name,
                          ((struct SendLocales *)b)->lang_name);
}


/* create_lang_menu:
   create language menu for the compose window. The order cannot be
   hardcoded because it depends on the current locale.

   Returns the current locale if any dictionaries were found
   and the menu was created;
   returns NULL if no dictionaries were found,
   in which case spell-checking must be disabled.
 */
#define BALSA_LANGUAGE_MENU_LANG "balsa-language-menu-lang"
#if !HAVE_GSPELL && !HAVE_GTKSPELL_3_0_3
static void
sw_broker_cb(const gchar *lang_tag,
             const gchar *provider_name,
             const gchar *provider_desc,
             const gchar *provider_file,
             gpointer     data)
{
    GList **lang_list = data;

    *lang_list = g_list_insert_sorted(*lang_list, g_strdup(lang_tag),
                                      (GCompareFunc) strcmp);
}


#endif                          /* HAVE_GTKSPELL_3_0_3 */

static const gchar *
create_lang_menu(GtkWidget    *parent,
                 BalsaComposeWindow *compose_window)
{
    guint i;
    GtkWidget *langs;
    static gboolean locales_sorted = FALSE;
    GSList *group                  = NULL;
#if HAVE_GSPELL
    const GList *lang_list, *l;
#else
    GList *lang_list, *l;
#endif                          /* HAVE_GSPELL */
#if !HAVE_GSPELL && !HAVE_GTKSPELL_3_0_3
    EnchantBroker *broker;
#endif                          /* HAVE_GTKSPELL_3_0_3 */
    const gchar *preferred_lang;
    GtkWidget *active_item = NULL;

#if HAVE_GTKSPELL_3_0_3
    lang_list = gtk_spell_checker_get_language_list();
#elif HAVE_GSPELL
    lang_list = gspell_language_get_available();
#else                           /* HAVE_GTKSPELL_3_0_3 */
    broker    = enchant_broker_init();
    lang_list = NULL;
    enchant_broker_list_dicts(broker, sw_broker_cb, &lang_list);
    enchant_broker_free(broker);
#endif                          /* HAVE_GTKSPELL_3_0_3 */

    if (lang_list == NULL)
        return NULL;


    if (!locales_sorted) {
        for (i = 0; i < G_N_ELEMENTS(locales); i++) {
            locales[i].lang_name = _(locales[i].lang_name);
        }
        qsort(locales, G_N_ELEMENTS(locales), sizeof(struct SendLocales),
              comp_send_locales);
        locales_sorted = TRUE;
    }

    /* find the preferred charset... */
    preferred_lang = balsa_app.spell_check_lang ?
        balsa_app.spell_check_lang : setlocale(LC_CTYPE, NULL);

    langs = gtk_menu_new();
    for (i = 0; i < G_N_ELEMENTS(locales); i++) {
        gconstpointer found;

        if ((locales[i].locale == NULL) || (locales[i].locale[0] == '\0'))
            /* GtkSpell handles NULL lang, but complains about empty
             * lang; in either case, it does not go in the langs menu. */
            continue;

#if HAVE_GSPELL
        found = gspell_language_lookup(locales[i].locale);
#else                           /* HAVE_GSPELL */
        found = g_list_find_custom(lang_list, locales[i].locale,
                                   (GCompareFunc) strcmp);
#endif                          /* HAVE_GSPELL */
        if (found != NULL) {
            GtkWidget *w;

            w = gtk_radio_menu_item_new_with_mnemonic(group,
                                                      locales[i].
                                                      lang_name);
            group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(w));
            g_signal_connect(G_OBJECT(w), "activate",
                             G_CALLBACK(lang_set_cb), compose_window);
            g_object_set_data_full(G_OBJECT(w), BALSA_LANGUAGE_MENU_LANG,
                                   g_strdup(locales[i].locale), g_free);
            gtk_menu_shell_append(GTK_MENU_SHELL(langs), w);

            if (!active_item || (strcmp(preferred_lang, locales[i].locale) == 0))
                active_item = w;
        }
    }

    /* Add to the langs menu any available languages that are
     * not listed in locales[] */
    for (l = lang_list; l; l = l->next) {
#if HAVE_GSPELL
        const GspellLanguage *language = l->data;
        const gchar *lang              = gspell_language_get_code(language);
#else                           /* HAVE_GSPELL */
        const gchar *lang = l->data;
#endif                          /* HAVE_GSPELL */
        gint j;

        j = find_locale_index_by_locale(lang);
        if ((j < 0) || (strcmp(lang, locales[j].locale) != 0)) {
            GtkWidget *w;

            w     = gtk_radio_menu_item_new_with_label(group, lang);
            group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(w));
            g_signal_connect(G_OBJECT(w), "activate",
                             G_CALLBACK(lang_set_cb), compose_window);
            g_object_set_data_full(G_OBJECT(w), BALSA_LANGUAGE_MENU_LANG,
                                   g_strdup(lang), g_free);
            gtk_menu_shell_append(GTK_MENU_SHELL(langs), w);

            if (!active_item || (strcmp(preferred_lang, lang) == 0))
                active_item = w;
        }
    }
#if !HAVE_GSPELL
    g_list_free_full(lang_list, (GDestroyNotify) g_free);
#endif                          /* HAVE_GSPELL */

    g_signal_handlers_block_by_func(active_item, lang_set_cb, compose_window);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(active_item), TRUE);
    g_signal_handlers_unblock_by_func(active_item, lang_set_cb, compose_window);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(parent), langs);

    return g_object_get_data(G_OBJECT(active_item), BALSA_LANGUAGE_MENU_LANG);
}


/* Standard buttons; "" means a separator. */
static const BalsaToolbarEntry compose_toolbar[] = {
    { "toolbar-send",
      BALSA_PIXMAP_SEND                                                                                           },
    { "",
      ""                                                                                                                       },
    { "attach-file",
      BALSA_PIXMAP_ATTACHMENT                                                                                                                                                                },
    { "",
      ""                                                                                                                                                                                                 },
    { "save",        "document-save"                                                                                                                                                                                                },
    { "",            ""                                                                                                                                                                                                             },
    { "undo",        "edit-undo"                                                                                                                                                                                                    },
    { "redo",        "edit-redo"                                                                                                                                                                                                    },
    { "",            ""                                                                                                                                                                                                             },
    { "select-ident",BALSA_PIXMAP_IDENTITY                                                                                                                                                                                          },
    { "",            ""                                                                                                                                                                                                             },
    { "spell-check", "tools-check-spelling"                                                                                                                                                                                         },
    { "",            ""                                                                                                                                                                                                             },
    {"print",        "document-print"                                                                                                                                                                                               },
    { "",            ""                                                                                                                                                                                                             },
    {"close",        "window-close-symbolic"                                                                                                                                                                                        }
};

/* Optional extra buttons */
static const BalsaToolbarEntry compose_toolbar_extras[] = {
    { "postpone",    BALSA_PIXMAP_POSTPONE                                                                                                              },
    { "request-mdn", BALSA_PIXMAP_REQUEST_MDN                                                                                                           },
#ifdef HAVE_GPGME
    { "sign",        BALSA_PIXMAP_GPG_SIGN                                                                                                              },
    { "encrypt",     BALSA_PIXMAP_GPG_ENCRYPT                                                                                                           },
#endif /* HAVE_GPGME */
    { "edit",        "gtk-edit"                                                                                                                         },
    { "queue",       BALSA_PIXMAP_QUEUE                                                                                                                 }
};

/* Create the toolbar model for the compose window's toolbar.
 */
BalsaToolbarModel *
balsa_compose_window_get_toolbar_model(void)
{
    static BalsaToolbarModel *model = NULL;

    if (model)
        return model;

    model =
        balsa_toolbar_model_new(BALSA_TOOLBAR_TYPE_COMPOSE_WINDOW,
                                compose_toolbar,
                                G_N_ELEMENTS(compose_toolbar));
    balsa_toolbar_model_add_entries(model, compose_toolbar_extras,
                                    G_N_ELEMENTS(compose_toolbar_extras));

    return model;
}


static void
compose_window_identities_changed_cb(BalsaComposeWindow *compose_window)
{
    sw_action_set_enabled(compose_window, "SelectIdentity",
                          balsa_app.identities->next != NULL);
}


static void
sw_cc_add_list(InternetAddressList **new_cc,
               InternetAddressList  *list)
{
    int i;

    if (!list)
        return;

    for (i = 0; i < internet_address_list_length(list); i++) {
        InternetAddress *ia = internet_address_list_get_address (list, i);
        GList *ilist;

        /* do not insert any of my identities into the cc: list */
        for (ilist = balsa_app.identities; ilist != NULL; ilist = ilist->next) {
            if (libbalsa_ia_rfc2821_equal
                    (ia, libbalsa_identity_get_address(ilist->data)))
                break;
        }
        if (ilist == NULL) {
            if (*new_cc == NULL)
                *new_cc = internet_address_list_new();
            internet_address_list_add(*new_cc, ia);
        }
    }
}


static void
insert_initial_sig(BalsaComposeWindow *compose_window)
{
    GtkTextIter sig_pos;
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));

    if (libbalsa_identity_get_sig_prepend(compose_window->ident))
        gtk_text_buffer_get_start_iter(buffer, &sig_pos);
    else
        gtk_text_buffer_get_end_iter(buffer, &sig_pos);
    gtk_text_buffer_insert(buffer, &sig_pos, "\n", 1);
    sw_insert_sig_activated(NULL, NULL, compose_window);
    gtk_text_buffer_get_start_iter(buffer, &sig_pos);
    gtk_text_buffer_place_cursor(buffer, &sig_pos);
}


static void
bsm_prepare_for_setup(LibBalsaMessage *message)
{
    LibBalsaMailbox *mailbox;

    mailbox = libbalsa_message_get_mailbox(message);
    if (mailbox != NULL)
        libbalsa_mailbox_open(mailbox, NULL);
    /* fill in that info:
     * ref the message so that we have all needed headers */
    libbalsa_message_body_ref(message, TRUE, TRUE);
#ifdef HAVE_GPGME
    /* scan the message for encrypted parts - this is only possible if
       there is *no* other ref to it */
    balsa_message_perform_crypto(message, LB_MAILBOX_CHK_CRYPT_NEVER,
                                 TRUE, 1);
#endif
}


/* libbalsa_message_body_unref() may destroy the @param part - this is
   why body_unref() is done at the end. */
static void
bsm_finish_setup(BalsaComposeWindow        *compose_window,
                 LibBalsaMessageBody *part)
{
    g_return_if_fail(part != NULL);
    g_return_if_fail(part->message != NULL);

    if (!compose_window->parent_message && !compose_window->draft_message) {
        LibBalsaMailbox *mailbox;

        mailbox = libbalsa_message_get_mailbox(part->message);
        if (mailbox != NULL)
            libbalsa_mailbox_close(mailbox, FALSE);
    }

    /* ...but mark it as unmodified. */
    compose_window->state = SENDMSG_STATE_CLEAN;
    compose_window_set_subject_from_body(compose_window, part, compose_window->ident);
    libbalsa_message_body_unref(part->message);
}


static void
set_cc_from_all_recipients(BalsaComposeWindow           *compose_window,
                           LibBalsaMessageHeaders *headers)
{
    InternetAddressList *new_cc = NULL;

    sw_cc_add_list(&new_cc, headers->to_list);
    sw_cc_add_list(&new_cc, headers->cc_list);

    libbalsa_address_view_set_from_list(compose_window->recipient_view,
                                        "CC:",
                                        new_cc);
    if (new_cc)
        g_object_unref(new_cc);
}


static void
set_in_reply_to(BalsaComposeWindow           *compose_window,
                const gchar            *message_id,
                LibBalsaMessageHeaders *headers)
{
    gchar *tmp;

    g_assert(message_id);
    if (message_id[0] == '<')
        tmp = g_strdup(message_id);
    else
        tmp = g_strconcat("<", message_id, ">", NULL);
    if (headers && headers->from) {
        gchar recvtime[50];

        ctime_r(&headers->date, recvtime);
        if (recvtime[0]) /* safety check; remove trailing '\n' */
            recvtime[strlen(recvtime) - 1] = '\0';
        compose_window->in_reply_to =
            g_strconcat(tmp, " (from ",
                        libbalsa_address_get_mailbox_from_list
                            (headers->from),
                        " on ", recvtime, ")", NULL);
        g_free(tmp);
    } else {
        compose_window->in_reply_to = tmp;
    }
}


static void
set_to(BalsaComposeWindow           *compose_window,
       LibBalsaMessageHeaders *headers)
{
    if (compose_window->type == SEND_REPLY_GROUP) {
        set_list_post_address(compose_window);
    } else {
        InternetAddressList *addr = headers->reply_to ?
            headers->reply_to : headers->from;

        libbalsa_address_view_set_from_list(compose_window->recipient_view,
                                            "To:", addr);
    }
}


static void
set_references_reply(BalsaComposeWindow *compose_window,
                     GList        *references,
                     const gchar  *in_reply_to,
                     const gchar  *message_id)
{
    GList *refs = NULL, *list;

    for (list = references; list; list = list->next) {
        refs = g_list_prepend(refs, g_strdup(list->data));
    }

    /* We're replying to parent_message, so construct the
     * references according to RFC 2822. */
    if (!references
        /* Parent message has no References header... */
        && in_reply_to)
        /* ...but it has an In-Reply-To header with a single
         * message identifier. */
        refs = g_list_prepend(refs, g_strdup(in_reply_to));
    if (message_id)
        refs = g_list_prepend(refs, g_strdup(message_id));

    compose_window->references = g_list_reverse(refs);
}


static void
set_identity(BalsaComposeWindow    *compose_window,
             LibBalsaMessage *message)
{
    /* Set up the default identity */
    if (!set_identity_from_mailbox(compose_window, message))
        /* Get the identity from the To: field of the original message */
        guess_identity(compose_window, message);
    /* From: */
    setup_headers_from_identity(compose_window, compose_window->ident);
}


static gboolean
sw_grab_focus_to_text(GtkWidget *text)
{
    gtk_widget_grab_focus(text);
    g_object_unref(text);
    return FALSE;
}


/* decode_and_strdup:
   decodes given URL string up to the delimiter and places the
   eos pointer in newstr if supplied (eos==NULL if end of string was reached)
 */
static gchar *
decode_and_strdup(const gchar *str,
                  int          delim,
                  gchar      **newstr)
{
    gchar num[3];
    GString *s = g_string_new(NULL);
    /* eos points to the character after the last to parse */
    gchar *eos = strchr(str, delim);

    if (!eos) eos = (gchar *)str + strlen(str);
    while (str < eos) {
        switch (*str) {
        case '+':
            g_string_append_c(s, ' ');
            str++;
            break;

        case '%':
            if (str + 2 < eos) {
                strncpy(num, str + 1, 2);
                num[2] = 0;
                g_string_append_c(s, strtol(num, NULL, 16));
            }
            str += 3;
            break;

        default:
            g_string_append_c(s, *str++);
        }
    }
    if (newstr) *newstr = *eos ? eos + 1 : NULL;
    eos = s->str;
    g_string_free(s, FALSE);
    return eos;
}


/* process_url:
   extracts all characters until NUL or question mark; parse later fields
   of format 'key'='value' with ampersands as separators.
 */
void
balsa_compose_window_process_url(const char  *url,
                           field_setter func,
                           void        *data)
{
    gchar *ptr, *to, *key, *val;

    to = decode_and_strdup(url, '?', &ptr);
    func(data, "to", to);
    g_free(to);
    while (ptr) {
        key = decode_and_strdup(ptr, '=', &ptr);
        if (ptr) {
            val = decode_and_strdup(ptr, '&', &ptr);
            func(data, key, val);
            g_free(val);
        }
        g_free(key);
    }
}


/* balsa_compose_window_set_field:
   sets given field of the compose window to the specified value.
 */

#define NO_SECURITY_ISSUES_WITH_ATTACHMENTS TRUE
#if defined(NO_SECURITY_ISSUES_WITH_ATTACHMENTS)
static void
sw_attach_file(BalsaComposeWindow *compose_window,
               const gchar  *val)
{
    GtkFileChooser *attach;

    if (!g_path_is_absolute(val)) {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_WARNING,
                                   _("Could not attach the file %s: %s."), val,
                                   _("not an absolute path"));
        return;
    }
    if (!(g_str_has_prefix(val, g_get_home_dir())
          || g_str_has_prefix(val, g_get_tmp_dir()))) {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_WARNING,
                                   _("Could not attach the file %s: %s."), val,
                                   _("not in your directory"));
        return;
    }
    if (!g_file_test(val, G_FILE_TEST_EXISTS)) {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_WARNING,
                                   _("Could not attach the file %s: %s."), val,
                                   _("does not exist"));
        return;
    }
    if (!g_file_test(val, G_FILE_TEST_IS_REGULAR)) {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_WARNING,
                                   _("Could not attach the file %s: %s."), val,
                                   _("not a regular file"));
        return;
    }
    attach = g_object_get_data(G_OBJECT(compose_window),
                               "balsa-sendmsg-window-attach-dialog");
    if (!attach) {
        attach = sw_attach_dialog(compose_window);
        g_object_set_data(G_OBJECT(compose_window),
                          "balsa-sendmsg-window-attach-dialog", attach);
        g_object_set_data_full(G_OBJECT(attach),
                               "balsa-sendmsg-window-attach-dir",
                               g_path_get_dirname(val), g_free);
    } else {
        gchar *dirname = g_object_get_data(G_OBJECT(attach),
                                           "balsa-sendmsg-window-attach-dir");
        gchar *valdir = g_path_get_dirname(val);
        gboolean good = (strcmp(dirname, valdir) == 0);

        g_free(valdir);
        if (!good) {
            /* gtk_file_chooser_select_filename will crash */
            balsa_information_parented(GTK_WINDOW(compose_window),
                                       LIBBALSA_INFORMATION_WARNING,
                                       _("Could not attach the file %s: %s."), val,
                                       _("not in current directory"));
            return;
        }
    }
    gtk_file_chooser_select_filename(attach, val);
}


#endif

void
balsa_compose_window_set_field(BalsaComposeWindow *compose_window,
                         const gchar  *key,
                         const gchar  *val)
{
    const gchar *type;
    g_return_if_fail(compose_window);

    if (g_ascii_strcasecmp(key, "body") == 0) {
        GtkTextBuffer *buffer =
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));

        gtk_text_buffer_insert_at_cursor(buffer, val, -1);

        return;
    }
#if defined(NO_SECURITY_ISSUES_WITH_ATTACHMENTS)
    if (g_ascii_strcasecmp(key, "attach") == 0) {
        sw_attach_file(compose_window, val);
        return;
    }
#endif
    if (g_ascii_strcasecmp(key, "subject") == 0) {
        append_comma_separated(GTK_EDITABLE(compose_window->subject[1]), val);
        return;
    }

    if (g_ascii_strcasecmp(key, "to") == 0) {
        type = "To:";
    } else if (g_ascii_strcasecmp(key, "cc") == 0) {
        type = "CC:";
    } else if (g_ascii_strcasecmp(key, "bcc") == 0) {
        type = "BCC:";
        if (!g_object_get_data(G_OBJECT(compose_window),
                               "balsa-sendmsg-window-url-bcc")) {
            GtkWidget *dialog =
                gtk_message_dialog_new
                    (GTK_WINDOW(compose_window),
                    GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_INFO,
                    GTK_BUTTONS_OK,
                    _("The link that you selected created\n"
                      "a “Blind copy” (BCC) address.\n"
                      "Please check that the address\n"
                      "is appropriate."));
#if HAVE_MACOSX_DESKTOP
            libbalsa_macosx_menu_for_parent(dialog, GTK_WINDOW(compose_window));
#endif
            g_object_set_data(G_OBJECT(compose_window),
                              "balsa-sendmsg-window-url-bcc", dialog);
            g_signal_connect(G_OBJECT(dialog), "response",
                             G_CALLBACK(gtk_widget_destroy), NULL);
            gtk_widget_show(dialog);
        }
    } else if (g_ascii_strcasecmp(key, "replyto") == 0) {
        libbalsa_address_view_add_from_string(compose_window->replyto_view,
                                              "Reply To:",
                                              val);
        return;
    } else {
        return;
    }

    libbalsa_address_view_add_from_string(compose_window->recipient_view, type, val);
}


/* opens the load file dialog box, allows selection of the file and includes
   it at current point */

static void
do_insert_string_select_ch(BalsaComposeWindow  *compose_window,
                           GtkTextBuffer *buffer,
                           const gchar   *string,
                           size_t         len,
                           const gchar   *fname)
{
    const gchar *charset       = NULL;
    LibBalsaTextAttribute attr = libbalsa_text_attr_string(string);

    do {
        LibBalsaCodeset codeset;
        LibBalsaCodesetInfo *info;
        gchar *s;

        if ((codeset = sw_get_user_codeset(compose_window, NULL, NULL, fname))
            == (LibBalsaCodeset) (-1))
            break;
        info = &libbalsa_codeset_info[codeset];

        charset = info->std;
        if (info->win && (attr & LIBBALSA_TEXT_HI_CTRL))
            charset = info->win;

        g_print("Trying charset: %s\n", charset);
        if (sw_can_convert(string, len, "UTF-8", charset, &s)) {
            gtk_text_buffer_insert_at_cursor(buffer, s, -1);
            g_free(s);
            break;
        }
    } while (1);
}


static void
insert_file_response(GtkWidget    *selector,
                     gint          response,
                     BalsaComposeWindow *compose_window)
{
    GtkFileChooser *fc;
    gchar *fname;

    if (response != GTK_RESPONSE_OK) {
        gtk_widget_destroy(selector);
        return;
    }

    fc    = GTK_FILE_CHOOSER(selector);
    fname = gtk_file_chooser_get_filename(fc);
    if (fname != NULL) {
        gchar *string;
        gsize len;
        GError *error = NULL;

        if (g_file_get_contents(fname, &string, &len, &error)) {
            LibBalsaTextAttribute attr;
            GtkTextBuffer *buffer;

            buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
            attr   = libbalsa_text_attr_string(string);
            if (!attr || (attr & LIBBALSA_TEXT_HI_UTF8)) {
                /* Ascii or utf-8 */
                gtk_text_buffer_insert_at_cursor(buffer, string, -1);
            } else {
                /* Neither ascii nor utf-8... */
                gchar *s             = NULL;
                const gchar *charset = sw_preferred_charset(compose_window);

                if (sw_can_convert(string, -1, "UTF-8", charset, &s)) {
                    /* ...but seems to be in current charset. */
                    gtk_text_buffer_insert_at_cursor(buffer, s, -1);
                    g_free(s);
                } else {
                    /* ...and can't be decoded from current charset. */
                    do_insert_string_select_ch(compose_window, buffer, string, len, fname);
                }
            }
            g_free(string);

            /* Use the same folder as for attachments. */
            g_free(balsa_app.attach_dir);
            balsa_app.attach_dir = gtk_file_chooser_get_current_folder(fc);
        } else {
            balsa_information_parented(GTK_WINDOW(compose_window),
                                       LIBBALSA_INFORMATION_WARNING,
                                       _(
                                           "Cannot not read the file “%s”: %s"), fname,
                                       error->message);
            g_error_free(error);
        }

        g_free(fname);
    }

    gtk_widget_destroy(selector);
}


static void
sw_include_file_activated(GSimpleAction *action,
                          GVariant      *parameter,
                          gpointer       data)
{
    BalsaComposeWindow *compose_window = data;
    GtkWidget *file_selector;

    file_selector =
        gtk_file_chooser_dialog_new(_("Include file"),
                                    GTK_WINDOW(compose_window),
                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                                    _("_OK"), GTK_RESPONSE_OK,
                                    NULL);
#if HAVE_MACOSX_DESKTOP
    libbalsa_macosx_menu_for_parent(file_selector, GTK_WINDOW(compose_window));
#endif
    gtk_window_set_destroy_with_parent(GTK_WINDOW(file_selector), TRUE);
    /* Use the same folder as for attachments. */
    if (balsa_app.attach_dir) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER
                                                (file_selector),
                                            balsa_app.attach_dir);
    }
    g_signal_connect(G_OBJECT(file_selector), "response",
                     G_CALLBACK(insert_file_response), compose_window);

    /* Display that dialog */
    gtk_widget_show(file_selector);
}


static void
strip_chars(gchar       *str,
            const gchar *char2strip)
{
    gchar *ins = str;
    while (*str) {
        if (strchr(char2strip, *str) == NULL)
            *ins++ = *str;
        str++;
    }
    *ins = '\0';
}


static void
sw_wrap_body(BalsaComposeWindow *compose_window)
{
    GtkTextView *text_view = GTK_TEXT_VIEW(compose_window->text);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(text_view);
    GtkTextIter start, end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);

    if (compose_window->flow) {
        sw_buffer_signals_block(compose_window, buffer);
        libbalsa_unwrap_buffer(buffer, &start, -1);
        sw_buffer_signals_unblock(compose_window, buffer);
    } else {
        GtkTextIter now;
        gint pos;
        gchar *the_text;

        gtk_text_buffer_get_iter_at_mark(buffer, &now,
                                         gtk_text_buffer_get_insert(buffer));
        pos = gtk_text_iter_get_offset(&now);

        the_text = gtk_text_iter_get_text(&start, &end);
        libbalsa_wrap_string(the_text, balsa_app.wraplength);
        gtk_text_buffer_set_text(buffer, "", 0);
        gtk_text_buffer_insert_at_cursor(buffer, the_text, -1);
        g_free(the_text);

        gtk_text_buffer_get_iter_at_offset(buffer, &now, pos);
        gtk_text_buffer_place_cursor(buffer, &now);
    }
    compose_window->state = SENDMSG_STATE_MODIFIED;
    gtk_text_view_scroll_to_mark(text_view,
                                 gtk_text_buffer_get_insert(buffer),
                                 0, FALSE, 0, 0);
}


static gboolean
attachment2message(GtkTreeModel *model,
                   GtkTreePath  *path,
                   GtkTreeIter  *iter,
                   gpointer      data)
{
    LibBalsaMessage *message = LIBBALSA_MESSAGE(data);
    BalsaAttachInfo *attachment;
    LibBalsaMessageBody *body;

    /* get the attachment information */
    gtk_tree_model_get(model, iter, ATTACH_INFO_COLUMN, &attachment, -1);

    /* create the attachment */
    body           = libbalsa_message_body_new(message);
    body->file_uri = attachment->file_uri;
    if (attachment->file_uri)
        g_object_ref(attachment->file_uri);
    else
        body->filename = g_strdup(attachment->uri_ref);
    body->content_type = g_strdup(attachment->force_mime_type);
    body->charset      = g_strdup(attachment->charset);
    body->attach_mode  = attachment->mode;
    libbalsa_message_append_part(message, body);

    /* clean up */
    g_object_unref(attachment);
    return FALSE;
}


/* compose_window2message:
   creates Message struct based on given BalsaMessage
   stripping EOL chars is necessary - the GtkEntry fields can in principle
   contain them. Such characters might screw up message formatting
   (consider moving this code to mutt part).
 */

static void
sw_set_header_from_path(LibBalsaMessage *message,
                        const gchar     *header,
                        const gchar     *path,
                        const gchar     *error_format)
{
    gchar *content = NULL;
    GError *err    = NULL;

    if (path && !(content =
                      libbalsa_get_header_from_path(header, path, NULL,
                                                    &err))) {
        balsa_information(LIBBALSA_INFORMATION_WARNING,
                          error_format, path, err->message);
        g_error_free(err);
    }

    libbalsa_message_set_user_header(message, header, content);
    g_free(content);
}


static const gchar *
sw_required_charset(BalsaComposeWindow *compose_window,
                    const gchar  *text)
{
    const gchar *charset = "us-ascii";

    if (libbalsa_text_attr_string(text)) {
        charset = sw_preferred_charset(compose_window);
        if (!sw_can_convert(text, -1, charset, "UTF-8", NULL))
            charset = "UTF-8";
    }

    return charset;
}


static LibBalsaMessage *
compose_window2message(BalsaComposeWindow *compose_window)
{
    LibBalsaMessage *message;
    LibBalsaMessageHeaders *headers;
    LibBalsaMessageBody *body;
    gchar *tmp;
    GtkTextIter start, end;
    LibBalsaIdentity *ident = compose_window->ident;
    InternetAddress *ia     = libbalsa_identity_get_address(ident);
    GtkTextBuffer *buffer;
    GtkTextBuffer *new_buffer = NULL;

    message = libbalsa_message_new();

    headers       = libbalsa_message_get_headers(message);
    headers->from = internet_address_list_new ();
    internet_address_list_add(headers->from, ia);

    tmp = gtk_editable_get_chars(GTK_EDITABLE(compose_window->subject[1]), 0, -1);
    strip_chars(tmp, "\r\n");
    libbalsa_message_set_subject(message, tmp);
    g_free(tmp);

    headers->to_list =
        libbalsa_address_view_get_list(compose_window->recipient_view, "To:");

    headers->cc_list =
        libbalsa_address_view_get_list(compose_window->recipient_view, "CC:");

    headers->bcc_list =
        libbalsa_address_view_get_list(compose_window->recipient_view, "BCC:");


    /* get the fcc-box from the option menu widget */
    compose_window->fcc_url =
        g_strdup(balsa_mblist_mru_option_menu_get(compose_window->fcc[1]));

    headers->reply_to =
        libbalsa_address_view_get_list(compose_window->replyto_view, "Reply To:");

    if (compose_window->req_mdn)
        libbalsa_message_set_dispnotify(message, ia);
    libbalsa_message_set_request_dsn(message, compose_window->req_dsn);

    sw_set_header_from_path(message, "Face", libbalsa_identity_get_face_path(ident),
                            /* Translators: please do not translate Face. */
                            _("Could not load Face header file %s: %s"));
    sw_set_header_from_path(message, "X-Face", libbalsa_identity_get_x_face_path(ident),
                            /* Translators: please do not translate Face. */
                            _("Could not load X-Face header file %s: %s"));

    libbalsa_message_set_references(message, compose_window->references);
    compose_window->references = NULL; /* steal it */

    if (compose_window->in_reply_to != NULL) {
        libbalsa_message_set_in_reply_to(message,
                                         g_list_prepend(NULL,
                                                        g_strdup(compose_window->in_reply_to)));
    }

    body = libbalsa_message_body_new(message);

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
    gtk_text_buffer_get_bounds(buffer, &start, &end);

    if (compose_window->flow) {
        /* Copy the message text to a new buffer: */
        GtkTextTagTable *table;

        table      = gtk_text_buffer_get_tag_table(buffer);
        new_buffer = gtk_text_buffer_new(table);

        tmp = gtk_text_iter_get_text(&start, &end);
        gtk_text_buffer_set_text(new_buffer, tmp, -1);
        g_free(tmp);

        /* Remove spaces before a newline: */
        gtk_text_buffer_get_bounds(new_buffer, &start, &end);
        libbalsa_unwrap_buffer(new_buffer, &start, -1);
        gtk_text_buffer_get_bounds(new_buffer, &start, &end);
    }

    /* Copy the buffer text to the message: */
    body->buffer = gtk_text_iter_get_text(&start, &end);
    if (new_buffer)
        g_object_unref(new_buffer);

    if (compose_window->send_mp_alt) {
        body->html_buffer =
            libbalsa_text_to_html(libbalsa_message_get_subject(message), body->buffer,
                                  compose_window->spell_check_lang);
    }
    if (compose_window->flow) {
        body->buffer =
            libbalsa_wrap_rfc2646(body->buffer, balsa_app.wraplength,
                                  TRUE, FALSE, TRUE);
    }

    /* Ildar reports that, when a message contains both text/plain and
     * text/html parts, some broken MUAs use the charset from the
     * text/plain part to display the text/html part; the latter is
     * encoded as UTF-8 by add_mime_body_plain (send.c), so we'll use
     * the same encoding for the text/plain part.
     * http://bugzilla.gnome.org/show_bug.cgi?id=580704 */
    body->charset =
        g_strdup(compose_window->send_mp_alt ?
                 "UTF-8" : sw_required_charset(compose_window, body->buffer));
    libbalsa_message_append_part(message, body);

    /* add attachments */
    if (compose_window->tree_view)
        gtk_tree_model_foreach(BALSA_MSG_ATTACH_MODEL(compose_window),
                               attachment2message, message);

    headers->date = time(NULL);
#ifdef HAVE_GPGME
    if (balsa_app.has_openpgp || balsa_app.has_smime) {
        libbalsa_message_set_gpg_mode(message,
                                      (compose_window->gpg_mode & LIBBALSA_PROTECT_MODE) !=
                                      0 ? compose_window->gpg_mode : 0);
        libbalsa_message_set_att_pubkey(message, compose_window->attach_pubkey);
        libbalsa_message_set_identity(message, ident);
    } else {
        libbalsa_message_set_gpg_mode(message, 0);
        libbalsa_message_set_att_pubkey(message, FALSE);
    }
#endif

    /* remember the parent window */
    g_object_set_data(G_OBJECT(message), "parent-window",
                      GTK_WINDOW(compose_window));

    return message;
}


/* ask the user for a subject */
static gboolean
subject_not_empty(BalsaComposeWindow *compose_window)
{
    const gchar *subj;
    GtkWidget *no_subj_dialog;
    GtkWidget *dialog_vbox;
    GtkWidget *hbox;
    GtkWidget *image;
    GtkWidget *vbox;
    gchar *text_str;
    GtkWidget *label;
    GtkWidget *subj_entry;
    gint response;

    /* read the subject widget and verify that it is contains something else
       than spaces */
    subj = gtk_entry_get_text(GTK_ENTRY(compose_window->subject[1]));
    if (subj) {
        const gchar *p = subj;

        while (*p && g_unichar_isspace(g_utf8_get_char(p))) {
            p = g_utf8_next_char(p);
        }
        if (*p != '\0')
            return TRUE;
    }

    /* build the dialog */
    no_subj_dialog =
        gtk_dialog_new_with_buttons(_("No Subject"),
                                    GTK_WINDOW(compose_window),
                                    GTK_DIALOG_MODAL |
                                    libbalsa_dialog_flags(),
                                    _("_Cancel"), GTK_RESPONSE_CANCEL,
                                    _("_Send"), GTK_RESPONSE_OK,
                                    NULL);
    gtk_window_set_resizable (GTK_WINDOW (no_subj_dialog), FALSE);
    gtk_window_set_type_hint (GTK_WINDOW (no_subj_dialog), GDK_SURFACE_TYPE_HINT_DIALOG);

    dialog_vbox = gtk_dialog_get_content_area(GTK_DIALOG(no_subj_dialog));
    g_object_set(G_OBJECT(dialog_vbox), "margin", 6, NULL);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_vexpand(hbox, TRUE);
    gtk_box_pack_start (GTK_BOX (dialog_vbox), hbox);
    g_object_set(G_OBJECT(hbox), "margin", 6, NULL);

    image = gtk_image_new_from_icon_name("dialog-question");
    gtk_box_pack_start (GTK_BOX (hbox), image);
    gtk_widget_set_valign(image, GTK_ALIGN_START);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_pack_start (GTK_BOX (hbox), vbox);

    text_str = g_strdup_printf("<span weight=\"bold\" size=\"larger\">%s</span>\n\n%s",
                               _("You did not specify a subject for this message"),
                               _("If you would like to provide one, enter it below."));
    label = gtk_label_new (text_str);
    g_free(text_str);
    gtk_box_pack_start (GTK_BOX (vbox), label);
    gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_valign(label, GTK_ALIGN_START);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start (GTK_BOX (vbox), hbox);

    label = gtk_label_new (_("Subject:"));
    gtk_box_pack_start (GTK_BOX (hbox), label);

    subj_entry = gtk_entry_new ();
    gtk_entry_set_text(GTK_ENTRY(subj_entry), _("(no subject)"));
    gtk_widget_set_hexpand(subj_entry, TRUE);
    gtk_box_pack_start (GTK_BOX (hbox), subj_entry);
    gtk_entry_set_activates_default (GTK_ENTRY (subj_entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG (no_subj_dialog),
                                    GTK_RESPONSE_OK);

    gtk_widget_grab_focus (subj_entry);
    gtk_editable_select_region(GTK_EDITABLE(subj_entry), 0, -1);

    response = gtk_dialog_run(GTK_DIALOG(no_subj_dialog));

    /* always set the current string in the subject entry */
    gtk_entry_set_text(GTK_ENTRY(compose_window->subject[1]),
                       gtk_entry_get_text(GTK_ENTRY(subj_entry)));
    gtk_widget_destroy(no_subj_dialog);

    return response == GTK_RESPONSE_OK;
}


#ifdef HAVE_GPGME
static gboolean
check_suggest_encryption(BalsaComposeWindow *compose_window)
{
    InternetAddressList *ia_list;
    gboolean can_encrypt;
    gpgme_protocol_t protocol;
    gint len;

    /* check if the user wants to see the message */
    if (!libbalsa_identity_get_warn_send_plain(compose_window->ident))
        return TRUE;

    /* nothing to do if encryption is already enabled */
    if ((compose_window->gpg_mode & LIBBALSA_PROTECT_ENCRYPT) != 0)
        return TRUE;

    /* we can not encrypt if we have bcc recipients */
    ia_list = libbalsa_address_view_get_list(compose_window->recipient_view, "BCC:");
    len     = internet_address_list_length(ia_list);
    g_object_unref(ia_list);
    if (len > 0)
        return TRUE;

    /* collect all to and cc recipients */
    protocol = compose_window->gpg_mode & LIBBALSA_PROTECT_SMIMEV3 ?
        GPGME_PROTOCOL_CMS : GPGME_PROTOCOL_OpenPGP;

    ia_list     = libbalsa_address_view_get_list(compose_window->recipient_view, "To:");
    can_encrypt = libbalsa_can_encrypt_for_all(ia_list, protocol);
    g_object_unref(ia_list);
    if (can_encrypt) {
        ia_list     = libbalsa_address_view_get_list(compose_window->recipient_view, "CC:");
        can_encrypt = libbalsa_can_encrypt_for_all(ia_list, protocol);
        g_object_unref(ia_list);
    }
    if (can_encrypt) {
        ia_list = internet_address_list_new();
        internet_address_list_add(ia_list, libbalsa_identity_get_address(compose_window->ident));
        can_encrypt = libbalsa_can_encrypt_for_all(ia_list, protocol);
        g_object_unref(ia_list);
    }

    /* ask the user if we could encrypt this message */
    if (can_encrypt) {
        GtkWidget *dialog;
        gint choice;
        GtkWidget *button;
        GtkWidget *hbox;
        GtkWidget *image;
        GtkWidget *label;

        dialog = gtk_message_dialog_new
                (GTK_WINDOW(compose_window),
                GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                GTK_MESSAGE_QUESTION,
                GTK_BUTTONS_NONE,
                _("You did not select encryption for this message, although "
                  "%s public keys are available for all recipients. In order "
                  "to protect your privacy, the message could be %s encrypted."),
                gpgme_get_protocol_name(protocol),
                gpgme_get_protocol_name(protocol));
#   if HAVE_MACOSX_DESKTOP
        libbalsa_macosx_menu_for_parent(dialog, GTK_WINDOW(compose_window));
#   endif


        button = gtk_button_new();
        gtk_dialog_add_action_widget(GTK_DIALOG(dialog), button, GTK_RESPONSE_YES);
        gtk_widget_set_can_default(button, TRUE);
        gtk_widget_grab_focus(button);

        hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
        gtk_container_add(GTK_CONTAINER(button), hbox);
        image = gtk_image_new_from_icon_name(balsa_icon_id(BALSA_PIXMAP_GPG_ENCRYPT));
        gtk_box_pack_start(GTK_BOX(hbox), image);
        label = gtk_label_new_with_mnemonic(_("Send _encrypted"));
        gtk_box_pack_start(GTK_BOX(hbox), label);

        button = gtk_button_new();
        gtk_dialog_add_action_widget(GTK_DIALOG(dialog), button, GTK_RESPONSE_NO);
        gtk_widget_set_can_default(button, TRUE);

        hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);
        gtk_container_add(GTK_CONTAINER(button), hbox);
        image = gtk_image_new_from_icon_name(balsa_icon_id(BALSA_PIXMAP_SEND));
        gtk_box_pack_start(GTK_BOX(hbox), image);
        label = gtk_label_new_with_mnemonic(_("Send _unencrypted"));
        gtk_box_pack_start(GTK_BOX(hbox), label);

        button = gtk_button_new_with_mnemonic(_("_Cancel"));
        gtk_dialog_add_action_widget(GTK_DIALOG(dialog), button, GTK_RESPONSE_CANCEL);
        gtk_widget_set_can_default(button, TRUE);

        choice = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (choice == GTK_RESPONSE_YES)
            compose_window_setup_gpg_ui_by_mode(compose_window, compose_window->gpg_mode | LIBBALSA_PROTECT_ENCRYPT);
        else if ((choice == GTK_RESPONSE_CANCEL) || (choice == GTK_RESPONSE_DELETE_EVENT))
            return FALSE;
    }

    return TRUE;
}


#endif

/* "send message" menu and toolbar callback.
 */
static gint
send_message_handler(BalsaComposeWindow *compose_window,
                     gboolean      queue_only)
{
    LibBalsaMsgCreateResult result;
    LibBalsaMessage *message;
    LibBalsaMailbox *fcc;
#ifdef HAVE_GPGME
    GtkTreeIter iter;
#endif
    GError *error = NULL;

    if (!compose_window->ready_to_send)
        return FALSE;

    if (!subject_not_empty(compose_window))
        return FALSE;

#ifdef HAVE_GPGME
    if (!check_suggest_encryption(compose_window))
        return FALSE;

    if ((compose_window->gpg_mode & LIBBALSA_PROTECT_OPENPGP) != 0) {
        gboolean warn_mp;
        gboolean warn_html_sign;

        warn_mp = (compose_window->gpg_mode & LIBBALSA_PROTECT_MODE) != 0 &&
            compose_window->tree_view &&
            gtk_tree_model_get_iter_first(BALSA_MSG_ATTACH_MODEL(compose_window), &iter);
        warn_html_sign = (compose_window->gpg_mode & LIBBALSA_PROTECT_MODE) == LIBBALSA_PROTECT_SIGN &&
            compose_window->send_mp_alt;

        if (warn_mp || warn_html_sign) {
            /* we are going to RFC2440 sign/encrypt a multipart, or to
             * RFC2440 sign a multipart/alternative... */
            GtkWidget *dialog;
            gint choice;
            GString *string =
                g_string_new(_("You selected OpenPGP security for this message.\n"));

            if (warn_html_sign)
                string =
                    g_string_append(string,
                                    _("The message text will be sent as plain text and as "
                                      "HTML, but only the plain part can be signed.\n"));
            if (warn_mp)
                string =
                    g_string_append(string,
                                    _("The message contains attachments, which cannot be "
                                      "signed or encrypted.\n"));
            string =
                g_string_append(string,
                                _("You should select MIME mode if the complete "
                                  "message shall be protected. Do you really want to proceed?"));
            dialog = gtk_message_dialog_new
                    (GTK_WINDOW(compose_window),
                    GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                    GTK_MESSAGE_QUESTION,
                    GTK_BUTTONS_OK_CANCEL, "%s", string->str);
#   if HAVE_MACOSX_DESKTOP
            libbalsa_macosx_menu_for_parent(dialog, GTK_WINDOW(compose_window));
#   endif
            g_string_free(string, TRUE);
            choice = gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            if (choice != GTK_RESPONSE_OK)
                return FALSE;
        }
    }
#endif

    message = compose_window2message(compose_window);
    fcc     = balsa_find_mailbox_by_url(compose_window->fcc_url);

#ifdef HAVE_GPGME
    balsa_information_parented(GTK_WINDOW(compose_window),
                               LIBBALSA_INFORMATION_DEBUG,
                               _("sending message with GPG mode %d"),
                               libbalsa_message_get_gpg_mode(message));
#endif

    if (queue_only) {
        result = libbalsa_message_queue(message, balsa_app.outbox, fcc,
                                        libbalsa_identity_get_smtp_server(compose_window->ident),
                                        compose_window->flow, &error);
    } else {
        result = libbalsa_message_send(message, balsa_app.outbox, fcc,
                                       balsa_find_sentbox_by_url,
                                       libbalsa_identity_get_smtp_server(compose_window->ident),
                                       balsa_app.send_progress_dialog,
                                       GTK_WINDOW(balsa_app.main_window),
                                       compose_window->flow, &error);
    }
    if (result == LIBBALSA_MESSAGE_CREATE_OK) {
        if (compose_window->parent_message != NULL) {
            LibBalsaMailbox *mailbox;

            mailbox = libbalsa_message_get_mailbox(compose_window->parent_message);
            if ((mailbox != NULL) &&
                !libbalsa_mailbox_get_readonly(mailbox))
                libbalsa_message_reply(compose_window->parent_message);
        }
        sw_delete_draft(compose_window);
    }

    g_object_unref(message);

    if (result != LIBBALSA_MESSAGE_CREATE_OK) {
        const char *msg;
        switch (result) {
        default:
        case LIBBALSA_MESSAGE_CREATE_ERROR:
            msg = _("Message could not be created");
            break;

        case LIBBALSA_MESSAGE_QUEUE_ERROR:
            msg = _("Message could not be queued in outbox");
            break;

        case LIBBALSA_MESSAGE_SAVE_ERROR:
            msg = _("Message could not be saved in sentbox");
            break;

        case LIBBALSA_MESSAGE_SEND_ERROR:
            msg = _("Message could not be sent");
            break;

#ifdef HAVE_GPGME
        case LIBBALSA_MESSAGE_SIGN_ERROR:
            msg = _("Message could not be signed");
            break;

        case LIBBALSA_MESSAGE_ENCRYPT_ERROR:
            msg = _("Message could not be encrypted");
            break;
#endif
        }
        if (error) {
            balsa_information_parented(GTK_WINDOW(compose_window),
                                       LIBBALSA_INFORMATION_ERROR,
                                       _("Send failed: %s\n%s"), msg,
                                       error->message);
        } else {
            balsa_information_parented(GTK_WINDOW(compose_window),
                                       LIBBALSA_INFORMATION_ERROR,
                                       _("Send failed: %s"), msg);
        }
        return FALSE;
    }
    g_clear_error(&error);

    gtk_widget_destroy((GtkWidget *) compose_window);

    return TRUE;
}


/* "send message" menu callback */
static void
sw_toolbar_send_activated(GSimpleAction *action,
                          GVariant      *parameter,
                          gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    send_message_handler(compose_window, balsa_app.always_queue_sent_mail);
}


static void
sw_send_activated(GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    send_message_handler(compose_window, FALSE);
}


static void
sw_queue_activated(GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    send_message_handler(compose_window, TRUE);
}


static gboolean
message_postpone(BalsaComposeWindow *compose_window)
{
    gboolean successp;
    LibBalsaMessage *message;
    GPtrArray *headers;
    GError *error = NULL;

    /* Silent fallback to UTF-8 */
    message = compose_window2message(compose_window);

    /* sufficiently long for fcc, mdn, gpg */
    headers = g_ptr_array_new();
    if (compose_window->fcc_url) {
        g_ptr_array_add(headers, g_strdup("X-Balsa-Fcc"));
        g_ptr_array_add(headers, g_strdup(compose_window->fcc_url));
    }
    g_ptr_array_add(headers, g_strdup("X-Balsa-MDN"));
    g_ptr_array_add(headers, g_strdup_printf("%d", compose_window->req_mdn));
    g_ptr_array_add(headers, g_strdup("X-Balsa-DSN"));
    g_ptr_array_add(headers, g_strdup_printf("%d", compose_window->req_dsn));
#ifdef HAVE_GPGME
    g_ptr_array_add(headers, g_strdup("X-Balsa-Crypto"));
    g_ptr_array_add(headers, g_strdup_printf("%d", compose_window->gpg_mode));
    g_ptr_array_add(headers, g_strdup("X-Balsa-Att-Pubkey"));
    g_ptr_array_add(headers, g_strdup_printf("%d", compose_window->attach_pubkey));
#endif

#if HAVE_GSPELL || HAVE_GTKSPELL
    if (sw_action_get_active(compose_window, "spell-check")) {
        g_ptr_array_add(headers, g_strdup("X-Balsa-Lang"));
        g_ptr_array_add(headers, g_strdup(compose_window->spell_check_lang));
    }
#else  /* HAVE_GTKSPELL */
    g_ptr_array_add(headers, g_strdup("X-Balsa-Lang"));
    g_ptr_array_add(headers, g_strdup(compose_window->spell_check_lang));
#endif /* HAVE_GTKSPELL */
    g_ptr_array_add(headers, g_strdup("X-Balsa-Format"));
    g_ptr_array_add(headers, g_strdup(compose_window->flow ? "Flowed" : "Fixed"));
    g_ptr_array_add(headers, g_strdup("X-Balsa-MP-Alt"));
    g_ptr_array_add(headers, g_strdup(compose_window->send_mp_alt ? "yes" : "no"));
    g_ptr_array_add(headers, g_strdup("X-Balsa-Send-Type"));
    g_ptr_array_add(headers, g_strdup_printf("%d", compose_window->type));
    g_ptr_array_add(headers, NULL);

    if (((compose_window->type == SEND_REPLY) || (compose_window->type == SEND_REPLY_ALL) ||
         (compose_window->type == SEND_REPLY_GROUP))) {
        successp = libbalsa_message_postpone(message, balsa_app.draftbox,
                                             compose_window->parent_message,
                                             (gchar **) headers->pdata,
                                             compose_window->flow, &error);
    } else {
        successp = libbalsa_message_postpone(message, balsa_app.draftbox,
                                             NULL,
                                             (gchar **) headers->pdata,
                                             compose_window->flow, &error);
    }
    g_ptr_array_foreach(headers, (GFunc) g_free, NULL);
    g_ptr_array_free(headers, TRUE);

    if (successp) {
        sw_delete_draft(compose_window);
    } else {
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_ERROR,
                                   _("Could not postpone message: %s"),
                                   error ? error->message : "");
        g_clear_error(&error);
    }

    g_object_unref(G_OBJECT(message));
    return successp;
}


/* "postpone message" menu callback */
static void
sw_postpone_activated(GSimpleAction *action,
                      GVariant      *parameter,
                      gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    if (compose_window->ready_to_send) {
        if (message_postpone(compose_window)) {
            balsa_information_parented(GTK_WINDOW(compose_window),
                                       LIBBALSA_INFORMATION_MESSAGE,
                                       _("Message postponed."));
            gtk_widget_destroy((GtkWidget *) compose_window);
        } else {
            balsa_information_parented(GTK_WINDOW(compose_window),
                                       LIBBALSA_INFORMATION_MESSAGE,
                                       _("Could not postpone message."));
        }
    }
}


static void
sw_save_activated(GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    if (sw_save_draft(compose_window))
        compose_window->state = SENDMSG_STATE_CLEAN;
}


static void
sw_page_setup_activated(GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       data)
{
    BalsaComposeWindow *compose_window = data;
    LibBalsaMessage *message;

    message = compose_window2message(compose_window);
    message_print_page_setup(GTK_WINDOW(compose_window));
    g_object_unref(message);
}


static void
sw_print_activated(GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
    BalsaComposeWindow *compose_window = data;
    LibBalsaMessage *message;

    message = compose_window2message(compose_window);
    message_print(message, GTK_WINDOW(compose_window));
    g_object_unref(message);
}


/*
 * Signal handlers for updating the cursor when text is inserted.
 * The "insert-text" signal is emitted before the insertion, so we
 * create a mark at the insertion point.
 * The "changed" signal is emitted after the insertion, and we move the
 * cursor to the end of the inserted text.
 * This achieves nothing if the text was typed, as the cursor is moved
 * there anyway; if the text is copied by drag and drop or center-click,
 * this deselects any selected text and places the cursor at the end of
 * the insertion.
 */
static void
sw_buffer_insert_text(GtkTextBuffer *buffer,
                      GtkTextIter   *iter,
                      const gchar   *text,
                      gint           len,
                      BalsaComposeWindow  *compose_window)
{
    compose_window->insert_mark =
        gtk_text_buffer_create_mark(buffer, "balsa-insert-mark", iter,
                                    FALSE);
#if !HAVE_GTKSOURCEVIEW
    /* If this insertion is not from the keyboard, or if we just undid
     * something, save the current buffer for undo. */
    if ((len > 1) /* Not keyboard? */
        || !sw_action_get_enabled(compose_window, "undo"))
        sw_buffer_save(compose_window);
#endif                          /* HAVE_GTKSOURCEVIEW */
}


static void
sw_buffer_changed(GtkTextBuffer *buffer,
                  BalsaComposeWindow  *compose_window)
{
    if (compose_window->insert_mark) {
        GtkTextIter iter;

        gtk_text_buffer_get_iter_at_mark(buffer, &iter,
                                         compose_window->insert_mark);
        gtk_text_buffer_place_cursor(buffer, &iter);
        compose_window->insert_mark = NULL;
    }

    compose_window->state = SENDMSG_STATE_MODIFIED;
}


#if !HAVE_GTKSOURCEVIEW
static void
sw_buffer_delete_range(GtkTextBuffer *buffer,
                       GtkTextIter   *start,
                       GtkTextIter   *end,
                       BalsaComposeWindow  *compose_window)
{
    if (gtk_text_iter_get_offset(end) >
        gtk_text_iter_get_offset(start) + 1)
        sw_buffer_save(compose_window);
}


#endif                          /* HAVE_GTKSOURCEVIEW */

/*
 * Helpers for the undo and redo buffers.
 */
static void
sw_buffer_signals_connect(BalsaComposeWindow *compose_window)
{
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));

    compose_window->insert_text_sig_id =
        g_signal_connect(buffer, "insert-text",
                         G_CALLBACK(sw_buffer_insert_text), compose_window);
    compose_window->changed_sig_id =
        g_signal_connect(buffer, "changed",
                         G_CALLBACK(sw_buffer_changed), compose_window);
#if !HAVE_GTKSOURCEVIEW
    compose_window->delete_range_sig_id =
        g_signal_connect(buffer, "delete-range",
                         G_CALLBACK(sw_buffer_delete_range), compose_window);
#endif                          /* HAVE_GTKSOURCEVIEW */
}


#if !HAVE_GTKSOURCEVIEW || !(HAVE_GSPELL || HAVE_GTKSPELL)
static void
sw_buffer_signals_disconnect(BalsaComposeWindow *compose_window)
{
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));

    g_signal_handler_disconnect(buffer, compose_window->changed_sig_id);
#   if !HAVE_GTKSOURCEVIEW
    g_signal_handler_disconnect(buffer, compose_window->delete_range_sig_id);
#   endif                       /* HAVE_GTKSOURCEVIEW */
    g_signal_handler_disconnect(buffer, compose_window->insert_text_sig_id);
}


#endif                          /* !HAVE_GTKSOURCEVIEW || !HAVE_GTKSPELL */

#if !HAVE_GTKSOURCEVIEW
static void
sw_buffer_set_undo(BalsaComposeWindow *compose_window,
                   gboolean      undo,
                   gboolean      redo)
{
    sw_action_set_enabled(compose_window, "undo", undo);
    sw_action_set_enabled(compose_window, "redo", redo);
}


#endif                          /* HAVE_GTKSOURCEVIEW */

#ifdef HAVE_GTKSPELL
/*
 * Callback for the spell-checker's "language-changed" signal.
 *
 * The signal is emitted if the user changes the spell-checker language
 * using the context menu.  If the new language is one that we have in
 * the menu, set the appropriate item active.
 */
static void
sw_spell_language_changed_cb(GtkSpellChecker *spell,
                             const gchar     *new_lang,
                             gpointer         data)
{
    BalsaComposeWindow *compose_window = data;
    GtkWidget *langs;
    GList *list, *children;

    langs = gtk_menu_item_get_submenu(GTK_MENU_ITEM
                                          (compose_window->current_language_menu));
    children = gtk_container_get_children(GTK_CONTAINER(langs));

    for (list = children; list; list = list->next) {
        GtkCheckMenuItem *menu_item = list->data;
        const gchar *lang;

        lang = g_object_get_data(G_OBJECT(menu_item),
                                 BALSA_LANGUAGE_MENU_LANG);
        if (strcmp(lang, new_lang) == 0) {
            g_signal_handlers_block_by_func(menu_item, lang_set_cb, compose_window);
            gtk_check_menu_item_set_active(menu_item, TRUE);
            g_signal_handlers_unblock_by_func(menu_item, lang_set_cb,
                                              compose_window);
            break;
        }
    }

    g_list_free(children);

    g_free(compose_window->spell_check_lang);
    compose_window->spell_check_lang = g_strdup(new_lang);
    g_free(balsa_app.spell_check_lang);
    balsa_app.spell_check_lang = g_strdup(new_lang);
}


static gboolean
sw_spell_detach(BalsaComposeWindow *compose_window)
{
    GtkSpellChecker *spell;

    spell = gtk_spell_checker_get_from_text_view(GTK_TEXT_VIEW(compose_window->text));
    if (spell)
        gtk_spell_checker_detach(spell);

    return spell != NULL;
}


static void
sw_spell_attach(BalsaComposeWindow *compose_window)
{
    GtkSpellChecker *spell;
    GError *err = NULL;

    /* Detach any existing spell checker */
    sw_spell_detach(compose_window);

    spell = gtk_spell_checker_new();
    gtk_spell_checker_set_language(spell, compose_window->spell_check_lang, &err);
    if (err) {
        /* Should not happen, since we now check the language. */
        balsa_information_parented(GTK_WINDOW(compose_window),
                                   LIBBALSA_INFORMATION_WARNING,
                                   _("Error starting spell checker: %s"),
                                   err->message);
        g_error_free(err);

        /* No spell checker, so deactivate the button. */
        sw_action_set_active(compose_window, "spell-check", FALSE);
    } else {
        gtk_spell_checker_attach(spell, GTK_TEXT_VIEW(compose_window->text));
        g_signal_connect(spell, "language-changed",
                         G_CALLBACK(sw_spell_language_changed_cb), compose_window);
    }
}


#endif                          /* HAVE_GTKSPELL */

#if !HAVE_GTKSOURCEVIEW
static void
sw_buffer_swap(BalsaComposeWindow *compose_window,
               gboolean      undo)
{
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
#   if HAVE_GTKSPELL
    gboolean had_spell;

    /* GtkSpell doesn't seem to handle setting a new buffer... */
    had_spell = sw_spell_detach(compose_window);
#   endif                       /* HAVE_GTKSPELL */

    sw_buffer_signals_disconnect(compose_window);
    g_object_ref(G_OBJECT(buffer));
    gtk_text_view_set_buffer(GTK_TEXT_VIEW(compose_window->text), compose_window->buffer2);
#   if HAVE_GTKSPELL
    if (had_spell)
        sw_spell_attach(compose_window);
#   endif                       /* HAVE_GTKSPELL */
    g_object_unref(compose_window->buffer2);
    compose_window->buffer2 = buffer;
    sw_buffer_signals_connect(compose_window);
    sw_buffer_set_undo(compose_window, !undo, undo);
}


static void
sw_buffer_save(BalsaComposeWindow *compose_window)
{
    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
    GtkTextIter start, end, iter;

    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gtk_text_buffer_set_text(compose_window->buffer2, "", 0);
    gtk_text_buffer_get_start_iter(compose_window->buffer2, &iter);
    gtk_text_buffer_insert_range(compose_window->buffer2, &iter, &start, &end);

    sw_buffer_set_undo(compose_window, TRUE, FALSE);
}


#endif                          /* HAVE_GTKSOURCEVIEW */

/*
 * Menu and toolbar callbacks.
 */

#if HAVE_GTKSOURCEVIEW
static void
sw_undo_activated(GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    g_signal_emit_by_name(compose_window->text, "undo");
}


static void
sw_redo_activated(GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    g_signal_emit_by_name(compose_window->text, "redo");
}


#else                           /* HAVE_GTKSOURCEVIEW */
static void
sw_undo_activated(GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    sw_buffer_swap(compose_window, TRUE);
}


static void
sw_redo_activated(GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    sw_buffer_swap(compose_window, FALSE);
}


#endif                          /* HAVE_GTKSOURCEVIEW */

/*
 * Cut, copy, and paste callbacks, and a helper.
 */
static void
clipboard_helper(BalsaComposeWindow *compose_window,
                 gchar        *signal)
{
    guint signal_id;
    GtkWidget *focus_widget =
        gtk_window_get_focus(GTK_WINDOW(compose_window));

    signal_id =
        g_signal_lookup(signal, G_TYPE_FROM_INSTANCE(focus_widget));
    if (signal_id)
        g_signal_emit(focus_widget, signal_id, (GQuark) 0);
}


static void
sw_cut_activated(GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       data)
{
    clipboard_helper(data, "cut-clipboard");
}


static void
sw_copy_activated(GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       data)
{
    clipboard_helper(data, "copy-clipboard");
}


static void
sw_paste_activated(GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       data)
{
    clipboard_helper(data, "paste-clipboard");
}


/*
 * More menu callbacks.
 */
static void
sw_select_text_activated(GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    balsa_window_select_all(GTK_WINDOW(compose_window));
}


static void
sw_wrap_body_activated(GSimpleAction *action,
                       GVariant      *parameter,
                       gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

#if !HAVE_GTKSOURCEVIEW
    sw_buffer_save(compose_window);
#endif                          /* HAVE_GTKSOURCEVIEW */
    sw_wrap_body(compose_window);
}


static void
sw_reflow_activated(GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       data)
{
    BalsaComposeWindow *compose_window = data;
    GtkTextView *text_view;
    GtkTextBuffer *buffer;
    GRegex *rex;

    if (!compose_window->flow)
        return;

    if (!(rex = balsa_quote_regex_new()))
        return;

#if !HAVE_GTKSOURCEVIEW
    sw_buffer_save(compose_window);
#endif                          /* HAVE_GTKSOURCEVIEW */

    text_view = GTK_TEXT_VIEW(compose_window->text);
    buffer    = gtk_text_view_get_buffer(text_view);
    sw_buffer_signals_block(compose_window, buffer);
    libbalsa_unwrap_selection(buffer, rex);
    sw_buffer_signals_unblock(compose_window, buffer);

    compose_window->state = SENDMSG_STATE_MODIFIED;
    gtk_text_view_scroll_to_mark(text_view,
                                 gtk_text_buffer_get_insert(buffer),
                                 0, FALSE, 0, 0);

    g_regex_unref(rex);
}


/* To field "changed" signal callback. */
static void
check_readiness(BalsaComposeWindow *compose_window)
{
    gboolean ready =
        libbalsa_address_view_n_addresses(compose_window->recipient_view) > 0;
    if (ready
        && (libbalsa_address_view_n_addresses(compose_window->replyto_view) < 0))
        ready = FALSE;

    compose_window->ready_to_send = ready;
    sw_actions_set_enabled(compose_window, ready_actions,
                           G_N_ELEMENTS(ready_actions), ready);
}


static const gchar *const header_action_names[] = {
    "from",
    "recips",
    "reply-to",
    "fcc"
};

/* sw_entry_helper:
   auxiliary function for "header show/hide" toggle menu entries.
   saves the show header configuration.
 */
static void
sw_entry_helper(GSimpleAction *action,
                GVariant      *state,
                BalsaComposeWindow  *compose_window,
                GtkWidget     *entry[])
{
    if (g_variant_get_boolean(state)) {
        gtk_widget_show(entry[0]);
        gtk_widget_show(entry[1]);
        gtk_widget_grab_focus(entry[1]);
    } else {
        gtk_widget_hide(entry[0]);
        gtk_widget_hide(entry[1]);
    }

    g_simple_action_set_state(G_SIMPLE_ACTION(action), state);

    if (compose_window->update_config) { /* then save the config */
        GString *str = g_string_new(NULL);
        unsigned i;

        for (i = 0; i < G_N_ELEMENTS(header_action_names); i++) {
            if (sw_action_get_active(compose_window, header_action_names[i])) {
                if (str->len > 0)
                    g_string_append_c(str, ' ');
                g_string_append(str, header_action_names[i]);
            }
        }
        g_free(balsa_app.compose_headers);
        balsa_app.compose_headers = g_string_free(str, FALSE);
    }

    g_simple_action_set_state(action, state);
}


static void
sw_from_change_state(GSimpleAction *action,
                     GVariant      *state,
                     gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    sw_entry_helper(action, state, compose_window, compose_window->from);
}


static void
sw_recips_change_state(GSimpleAction *action,
                       GVariant      *state,
                       gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    sw_entry_helper(action, state, compose_window, compose_window->recipients);
}


static void
sw_reply_to_change_state(GSimpleAction *action,
                         GVariant      *state,
                         gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    sw_entry_helper(action, state, compose_window, compose_window->replyto);
}


static void
sw_fcc_change_state(GSimpleAction *action,
                    GVariant      *state,
                    gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    sw_entry_helper(action, state, compose_window, compose_window->fcc);
}


static void
sw_request_mdn_change_state(GSimpleAction *action,
                            GVariant      *state,
                            gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    compose_window->req_mdn = g_variant_get_boolean(state);

    g_simple_action_set_state(action, state);
}


static void
sw_request_dsn_change_state(GSimpleAction *action,
                            GVariant      *state,
                            gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    compose_window->req_dsn = g_variant_get_boolean(state);

    g_simple_action_set_state(action, state);
}


static void
sw_show_toolbar_change_state(GSimpleAction *action,
                             GVariant      *state,
                             gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    balsa_app.show_compose_toolbar = g_variant_get_boolean(state);
    if (balsa_app.show_compose_toolbar)
        gtk_widget_show((GtkWidget *) compose_window->toolbar);
    else
        gtk_widget_hide((GtkWidget *) compose_window->toolbar);

    g_simple_action_set_state(action, state);
}


static void
sw_flowed_change_state(GSimpleAction *action,
                       GVariant      *state,
                       gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    compose_window->flow = g_variant_get_boolean(state);
    sw_action_set_enabled(compose_window, "reflow", compose_window->flow);

    g_simple_action_set_state(action, state);
}


static void
sw_send_html_change_state(GSimpleAction *action,
                          GVariant      *state,
                          gpointer       data)
{
    BalsaComposeWindow *compose_window = data;

    compose_window->send_mp_alt = g_variant_get_boolean(state);

    g_simple_action_set_state(action, state);
}


#ifdef HAVE_GPGME
static void
sw_gpg_helper(GSimpleAction *action,
              GVariant      *state,
              gpointer       data,
              guint          mask)
{
    BalsaComposeWindow *compose_window = data;
    gboolean butval, radio_on;

    butval = g_variant_get_boolean(state);
    if (butval)
        compose_window->gpg_mode |= mask;
    else
        compose_window->gpg_mode &= ~mask;

    radio_on = (compose_window->gpg_mode & LIBBALSA_PROTECT_MODE) > 0;
    sw_action_set_enabled(compose_window, "gpg-mode", radio_on);

    g_simple_action_set_state(action, state);
}


static void
sw_sign_change_state(GSimpleAction *action,
                     GVariant      *state,
                     gpointer       data)
{
    sw_gpg_helper(action, state, data, LIBBALSA_PROTECT_SIGN);
}


static void
sw_encrypt_change_state(GSimpleAction *action,
                        GVariant      *state,
                        gpointer       data)
{
    sw_gpg_helper(action, state, data, LIBBALSA_PROTECT_ENCRYPT);
}


static void
sw_att_pubkey_change_state(GSimpleAction *action,
                           GVariant      *state,
                           gpointer       data)
{
    BalsaComposeWindow *compose_window = (BalsaComposeWindow *) data;

    compose_window->attach_pubkey = g_variant_get_boolean(state);
    g_simple_action_set_state(action, state);
}


static void
sw_gpg_mode_change_state(GSimpleAction *action,
                         GVariant      *state,
                         gpointer       data)
{
    BalsaComposeWindow *compose_window = data;
    const gchar *mode;
    guint rfc_flag = 0;

    mode = g_variant_get_string(state, NULL);
    if (strcmp(mode, "mime") == 0) {
        rfc_flag = LIBBALSA_PROTECT_RFC3156;
    } else if (strcmp(mode, "open-pgp") == 0) {
        rfc_flag = LIBBALSA_PROTECT_OPENPGP;
    } else if (strcmp(mode, "smime") == 0) {
        rfc_flag = LIBBALSA_PROTECT_SMIMEV3;
    } else {
        g_print("%s unknown mode “%s”\n", __func__, mode);
        return;
    }

    compose_window->gpg_mode =
        (compose_window->gpg_mode & ~LIBBALSA_PROTECT_PROTOCOL) | rfc_flag;

    g_simple_action_set_state(action, state);
}


#endif                          /* HAVE_GPGME */


/* init_menus:
   performs the initial menu setup: shown headers as well as correct
   message charset.
 */
static void
init_menus(BalsaComposeWindow *compose_window)
{
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(header_action_names); i++) {
        gboolean found =
            libbalsa_find_word(header_action_names[i],
                               balsa_app.compose_headers);
        if (!found) {
            /* Be compatible with old action-names */
            struct {
                const gchar *old_action_name;
                const gchar *new_action_name;
            } name_map[] = {
                {"From",       "from"                                                                                                      },
                {"Recipients", "recips"                                                                                                    },
                {"ReplyTo",    "reply-to"                                                                                                  },
                {"Fcc",        "fcc"                                                                                                       }
            };
            guint j;

            for (j = 0; j < G_N_ELEMENTS(name_map); j++) {
                if (strcmp(header_action_names[i],
                           name_map[j].new_action_name) == 0) {
                    found =
                        libbalsa_find_word(name_map[j].old_action_name,
                                           balsa_app.compose_headers);
                }
            }
        }
        sw_action_set_active(compose_window, header_action_names[i], found);
    }

    /* gray 'send' and 'postpone' */
    check_readiness(compose_window);
}


static void
set_locale(BalsaComposeWindow *compose_window,
           const gchar  *locale)
{
#if HAVE_GSPELL
    if (sw_action_get_enabled(compose_window, "spell-check")) {
        const GspellLanguage *language;

        language = gspell_language_lookup(locale);
        if (language != NULL) {
            GtkTextBuffer *buffer;
            GspellTextBuffer *gspell_buffer;
            GspellChecker *checker;

            buffer        = gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
            gspell_buffer = gspell_text_buffer_get_from_gtk_text_buffer(buffer);
            checker       = gspell_text_buffer_get_spell_checker(gspell_buffer);
            gspell_checker_set_language(checker, language);
        }
    }

#endif                          /* HAVE_GSPELL */
    g_free(compose_window->spell_check_lang);
    compose_window->spell_check_lang = g_strdup(locale);

#if HAVE_GTKSPELL
    if (sw_action_get_enabled(compose_window, "spell-check")) {
        if (sw_spell_detach(compose_window))
            sw_spell_attach(compose_window);
    }
#endif                          /* HAVE_GTKSPELL */
}


#if HAVE_GSPELL || HAVE_GTKSPELL
/* spell_check_menu_cb
 *
 * Toggle the spell checker
 */
static void
sw_spell_check_change_state(GSimpleAction *action,
                            GVariant      *state,
                            gpointer       data)
{
    BalsaComposeWindow *compose_window = data;
#   if HAVE_GSPELL
    GtkTextView *text_view;
    GspellTextView *gspell_view;

    balsa_app.spell_check_active = g_variant_get_boolean(state);
    text_view                    = GTK_TEXT_VIEW(compose_window->text);
    gspell_view                  = gspell_text_view_get_from_gtk_text_view(text_view);
    gspell_text_view_set_inline_spell_checking(gspell_view,
                                               balsa_app.spell_check_active);
#   elif HAVE_GTKSPELL

    if ((balsa_app.spell_check_active = g_variant_get_boolean(state)))
        sw_spell_attach(compose_window);
    else
        sw_spell_detach(compose_window);
#   endif                       /* HAVE_GSPELL */

    g_simple_action_set_state(action, state);
}


#else                           /* HAVE_GTKSPELL */
/* spell_check_cb
 *
 * Start the spell check
 * */
static void
sw_spell_check_activated(GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       data)
{
    BalsaComposeWindow *compose_window    = data;
    GtkTextView *text_view = GTK_TEXT_VIEW(compose_window->text);
    BalsaSpellCheck *sc;

    if (compose_window->spell_checker) {
        if (gtk_widget_get_realized((GtkWidget *) compose_window->spell_checker)) {
            gtk_window_present(GTK_WINDOW(compose_window->spell_checker));
            return;
        } else {
            /* A spell checker was created, but not shown because of
             * errors; we'll destroy it, and create a new one. */
            gtk_widget_destroy((GtkWidget *) compose_window->spell_checker);
        }
    }

    sw_buffer_signals_disconnect(compose_window);

    compose_window->spell_checker = balsa_spell_check_new(GTK_WINDOW(compose_window));
    sc                   = BALSA_SPELL_CHECK(compose_window->spell_checker);

    /* configure the spell checker */
    balsa_spell_check_set_text(sc, text_view);
    balsa_spell_check_set_language(sc, compose_window->spell_check_lang);

    g_object_weak_ref(G_OBJECT(sc),
                      (GWeakNotify) sw_spell_check_weak_notify, compose_window);
    gtk_text_view_set_editable(text_view, FALSE);

    balsa_spell_check_start(sc);
}


static void
sw_spell_check_weak_notify(BalsaComposeWindow *compose_window)
{
    compose_window->spell_checker = NULL;
    gtk_text_view_set_editable(GTK_TEXT_VIEW(compose_window->text), TRUE);
    sw_buffer_signals_connect(compose_window);
}


#endif                          /* HAVE_GTKSPELL */

static void
lang_set_cb(GtkWidget    *w,
            BalsaComposeWindow *compose_window)
{
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w))) {
        const gchar *lang;

        lang = g_object_get_data(G_OBJECT(w), BALSA_LANGUAGE_MENU_LANG);
        set_locale(compose_window, lang);
        g_free(balsa_app.spell_check_lang);
        balsa_app.spell_check_lang = g_strdup(lang);
#if HAVE_GSPELL || HAVE_GTKSPELL
        sw_action_set_active(compose_window, "spell-check", TRUE);
#endif                          /* HAVE_GTKSPELL */
    }
}


/* balsa_compose_window_new_from_list:
 * like balsa_compose_window_new, but takes a GList of messages, instead of a
 * single message;
 * called by compose_from_list (balsa-index.c)
 */
BalsaComposeWindow *
balsa_compose_window_new_from_list(LibBalsaMailbox *mailbox,
                                   GArray          *selected,
                                   SendType         type)
{
    BalsaComposeWindow *compose_window;
    LibBalsaMessage *message;
    GtkTextBuffer *buffer;
    guint i;
    guint msgno;

    g_return_val_if_fail(selected->len > 0, NULL);

    msgno   = g_array_index(selected, guint, 0);
    message = libbalsa_mailbox_get_message(mailbox, msgno);
    if (message == NULL)
        return NULL;

    switch (type) {
    case SEND_FORWARD_ATTACH:
    case SEND_FORWARD_INLINE:
        compose_window = balsa_compose_window_forward(mailbox, msgno,
                                                      type == SEND_FORWARD_ATTACH);
        break;

    default:
        g_assert_not_reached(); /* since it hardly makes sense... */
        compose_window = NULL;           /** silence invalid warnings */

    }
    g_object_unref(message);

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));

    for (i = 1; i < selected->len; i++) {
        msgno   = g_array_index(selected, guint, i);
        message = libbalsa_mailbox_get_message(mailbox, msgno);
        if (message == NULL)
            continue;

        if (type == SEND_FORWARD_ATTACH) {
            attach_message(compose_window, message);
        } else if (type == SEND_FORWARD_INLINE) {
            GString *body =
                quote_message_body(compose_window, message, QUOTE_NOPREFIX);
            gtk_text_buffer_insert_at_cursor(buffer, body->str, body->len);
            g_string_free(body, TRUE);
        }
        g_object_unref(message);
    }

    compose_window->state = SENDMSG_STATE_CLEAN;

    return compose_window;
}


/* set_list_post_address:
 * look for the address for posting messages to a list */
static void
set_list_post_address(BalsaComposeWindow *compose_window)
{
    LibBalsaMessage *message =
        compose_window->parent_message ?
        compose_window->parent_message : compose_window->draft_message;
    const gchar *header;

    if ((header = libbalsa_message_get_user_header(message, "list-post"))
        && set_list_post_rfc2369(compose_window, header))
        return;

    /* we didn't find "list-post", so try some nonstandard
     * alternatives: */

    if ((header = libbalsa_message_get_user_header(message, "x-beenthere"))
        || (header =
                libbalsa_message_get_user_header(message, "x-mailing-list")))
        libbalsa_address_view_set_from_string(compose_window->recipient_view, "To:",
                                              header);
}


/* set_list_post_rfc2369:
 * look for "List-Post:" header, and get the address */
static gboolean
set_list_post_rfc2369(BalsaComposeWindow *compose_window,
                      const gchar  *url)
{
    /* RFC 2369: To allow for future extension, client
     * applications MUST follow the following guidelines for
     * handling the contents of the header fields described in
     * this document:
     * 1) Except where noted for specific fields, if the content
     *    of the field (following any leading whitespace,
     *    including comments) begins with any character other
     *    than the opening angle bracket '<', the field SHOULD
     *    be ignored.
     * 2) Any characters following an angle bracket enclosed URL
     *    SHOULD be ignored, unless a comma is the first
     *    non-whitespace/comment character after the closing
     *    angle bracket.
     * 3) If a sub-item (comma-separated item) within the field
     *    is not an angle-bracket enclosed URL, the remainder of
     *    the field (the current, and all subsequent sub-items)
     *    SHOULD be ignored. */
    /* RFC 2369: The client application should use the
     * left most protocol that it supports, or knows how to
     * access by a separate application. */
    while (*(url = rfc2822_skip_comments(url)) == '<') {
        const gchar *close = strchr(++url, '>');
        if (!close)
            /* broken syntax--break and return FALSE */
            break;
        if (g_ascii_strncasecmp(url, "mailto:", 7) == 0) {
            /* we support mailto! */
            gchar *field = g_strndup(&url[7], close - &url[7]);
            balsa_compose_window_process_url(field, balsa_compose_window_set_field,
                                       compose_window);
            g_free(field);
            return TRUE;
        }
        if (!(*++close && (*(close = rfc2822_skip_comments(close)) == ',')))
            break;
        url = ++close;
    }
    return FALSE;
}


/* rfc2822_skip_comments:
 * skip CFWS (comments and folding white space)
 *
 * CRLFs have already been stripped, so we need to look only for
 * comments and white space
 *
 * returns a pointer to the first character following the CFWS,
 * which may point to a '\0' character but is never a NULL pointer */
static const gchar *
rfc2822_skip_comments(const gchar *str)
{
    gint level = 0;

    while (*str) {
        if (*str == '(') {
            /* start of a comment--they nest */
            ++level;
        } else if (level > 0) {
            if (*str == ')')
                /* end of a comment */
                --level;
            else if ((*str == '\\') && (*++str == '\0'))
                /* quoted-pair: we must test for the end of the string,
                 * which would be an error; in this case, return a
                 * pointer to the '\0' character following the '\\' */
                break;
        } else if (!((*str == ' ') || (*str == '\t'))) {
            break;
        }
        ++str;
    }
    return str;
}


/* Set the title for the compose window;
 *
 * handler for the "changed" signals of the "To:" address and the
 * "Subject:" field;
 *
 * also called directly from balsa_compose_window_new.
 */
static void
balsa_compose_window_set_title(BalsaComposeWindow *compose_window)
{
    gchar *title_format;
    InternetAddressList *list;
    gchar *to_string;
    gchar *title;

    switch (compose_window->type) {
    case SEND_REPLY:
    case SEND_REPLY_ALL:
    case SEND_REPLY_GROUP:
        title_format = _("Reply to %s: %s");
        break;

    case SEND_FORWARD_ATTACH:
    case SEND_FORWARD_INLINE:
        title_format = _("Forward message to %s: %s");
        break;

    default:
        title_format = _("New message to %s: %s");
        break;
    }

    list      = libbalsa_address_view_get_list(compose_window->recipient_view, "To:");
    to_string = internet_address_list_to_string(list, FALSE);
    g_object_unref(list);

    title = g_strdup_printf(title_format, to_string ? to_string : "",
                            gtk_entry_get_text(GTK_ENTRY(compose_window->subject[1])));
    g_free(to_string);
    gtk_window_set_title(GTK_WINDOW(compose_window), title);
    g_free(title);
}


#ifdef HAVE_GPGME
static void
compose_window_update_gpg_ui_on_ident_change(BalsaComposeWindow     *compose_window,
                                    LibBalsaIdentity *ident)
{
    GAction *action;

    /* do nothing if we don't support crypto */
    if (!balsa_app.has_openpgp && !balsa_app.has_smime)
        return;

    action = sw_get_action(compose_window, "gpg-mode");

    /* preset according to identity */
    compose_window->gpg_mode = 0;
    if (libbalsa_identity_get_always_trust(ident))
        compose_window->gpg_mode |= LIBBALSA_PROTECT_ALWAYS_TRUST;

    sw_action_set_active(compose_window, "sign", libbalsa_identity_get_gpg_sign(ident));
    if (libbalsa_identity_get_gpg_sign(ident))
        compose_window->gpg_mode |= LIBBALSA_PROTECT_SIGN;

    sw_action_set_active(compose_window, "encrypt", libbalsa_identity_get_gpg_encrypt(ident));
    if (libbalsa_identity_get_gpg_encrypt(ident))
        compose_window->gpg_mode |= LIBBALSA_PROTECT_ENCRYPT;

    switch (libbalsa_identity_get_crypt_protocol(ident)) {
    case LIBBALSA_PROTECT_OPENPGP:
        compose_window->gpg_mode |= LIBBALSA_PROTECT_OPENPGP;
        g_action_change_state(action, g_variant_new_string("open-pgp"));
        break;

    case LIBBALSA_PROTECT_SMIMEV3:
        compose_window->gpg_mode |= LIBBALSA_PROTECT_SMIMEV3;
        g_action_change_state(action, g_variant_new_string("smime"));
        break;

    case LIBBALSA_PROTECT_RFC3156:
    default:
        compose_window->gpg_mode |= LIBBALSA_PROTECT_RFC3156;
        g_action_change_state(action, g_variant_new_string("mime"));
    }
}


static void
compose_window_setup_gpg_ui(BalsaComposeWindow *compose_window)
{
    /* make everything insensitive if we don't have crypto support */
    sw_action_set_enabled(compose_window, "gpg-mode", balsa_app.has_openpgp ||
                          balsa_app.has_smime);
    sw_action_set_enabled(compose_window, "attpubkey", balsa_app.has_openpgp);
}


static void
compose_window_setup_gpg_ui_by_mode(BalsaComposeWindow *compose_window,
                           gint          mode)
{
    GAction *action;

    /* do nothing if we don't support crypto */
    if (!balsa_app.has_openpgp && !balsa_app.has_smime)
        return;

    compose_window->gpg_mode = mode;
    sw_action_set_active(compose_window, "sign", mode & LIBBALSA_PROTECT_SIGN);
    sw_action_set_active(compose_window, "encrypt", mode & LIBBALSA_PROTECT_ENCRYPT);

    action = sw_get_action(compose_window, "gpg-mode");
    if (mode & LIBBALSA_PROTECT_SMIMEV3)
        g_action_change_state(action, g_variant_new_string("smime"));
    else if (mode & LIBBALSA_PROTECT_OPENPGP)
        g_action_change_state(action, g_variant_new_string("open-pgp"));
    else
        g_action_change_state(action, g_variant_new_string("mime"));
}


#endif /* HAVE_GPGME */

static GActionEntry win_entries[] = {
    {"include-file",
     sw_include_file_activated                                             },
    {"attach-file",
     sw_attach_file_activated                                              },
    {"include-messages",
     sw_include_messages_activated                                         },
    {"attach-messages",
     sw_attach_messages_activated                                          },
    {"send",
     sw_send_activated                                                     },
    {"queue",
     sw_queue_activated                                                    },
    {"postpone",
     sw_postpone_activated                                                 },
    {"save",
     sw_save_activated                                                     },
    {"page-setup",
     sw_page_setup_activated                                               },
    {"print",
     sw_print_activated                                                    },
    {"close",
     sw_close_activated                                                    },
    {"undo",
     sw_undo_activated                                                     },
    {"redo",
     sw_redo_activated                                                     },
    {"cut",
     sw_cut_activated                                                      },
    {"copy",
     sw_copy_activated                                                     },
    {"paste",
     sw_paste_activated                                                    },
    {"select-all",
     sw_select_text_activated                                              },
    {"wrap-body",
     sw_wrap_body_activated                                                },
    {"reflow",
     sw_reflow_activated                                                   },
    {"insert-sig",
     sw_insert_sig_activated                                               },
    {"quote",
     sw_quote_activated                                                    },
#if HAVE_GSPELL || HAVE_GTKSPELL
    {"spell-check",     libbalsa_toggle_activated,              NULL,              "false",
     sw_spell_check_change_state    },
#else                           /* HAVE_GTKSPELL */
    {"spell-check",
     sw_spell_check_activated                                              },
#endif                          /* HAVE_GTKSPELL */
    {"select-ident",
     sw_select_ident_activated                                             },
    {"edit",
     sw_edit_activated                                                     },
    {"show-toolbar",    libbalsa_toggle_activated,              NULL,              "false",
     sw_show_toolbar_change_state   },
    {"from",            libbalsa_toggle_activated,              NULL,              "false",
     sw_from_change_state           },
    {"recips",          libbalsa_toggle_activated,              NULL,              "false",
     sw_recips_change_state         },
    {"reply-to",        libbalsa_toggle_activated,              NULL,              "false",
     sw_reply_to_change_state       },
    {"fcc",             libbalsa_toggle_activated,              NULL,              "false",
     sw_fcc_change_state            },
    {"request-mdn",     libbalsa_toggle_activated,              NULL,              "false",
     sw_request_mdn_change_state    },
    {"request-dsn",     libbalsa_toggle_activated,              NULL,              "false",
     sw_request_dsn_change_state    },
    {"flowed",          libbalsa_toggle_activated,              NULL,              "false",
     sw_flowed_change_state         },
    {"send-html",       libbalsa_toggle_activated,              NULL,              "false",
     sw_send_html_change_state      },
#ifdef HAVE_GPGME
    {"sign",            libbalsa_toggle_activated,              NULL,              "false",
     sw_sign_change_state           },
    {"encrypt",         libbalsa_toggle_activated,              NULL,              "false",
     sw_encrypt_change_state        },
    {"gpg-mode",        libbalsa_radio_activated,               "s",               "'mime'",
     sw_gpg_mode_change_state       },
    {"gpg-mode",        libbalsa_radio_activated,               "s",               "'open-pgp'",
     sw_gpg_mode_change_state       },
    {"gpg-mode",        libbalsa_radio_activated,               "s",               "'smime'",
     sw_gpg_mode_change_state       },
    {"attpubkey",       libbalsa_toggle_activated,              NULL,              "false",
     sw_att_pubkey_change_state     },
#endif /* HAVE_GPGME */
    /* Only a toolbar button: */
    {"toolbar-send",    sw_toolbar_send_activated                                             }
};

void
balsa_compose_window_add_action_entries(GActionMap *action_map)
{
    g_action_map_add_action_entries(action_map, win_entries,
                                    G_N_ELEMENTS(win_entries), action_map);
}


static void
sw_menubar_foreach(GtkWidget *widget,
                   gpointer   data)
{
    GtkWidget **lang_menu = data;
    GtkMenuItem *item     = GTK_MENU_ITEM(widget);

    if (strcmp(gtk_menu_item_get_label(item), _("_Language")) == 0)
        *lang_menu = widget;
}


static BalsaComposeWindow *
balsa_compose_window_new()
{
    BalsaToolbarModel *model;
    GtkWidget *window;
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    BalsaComposeWindow *compose_window = NULL;
#if HAVE_GTKSOURCEVIEW
    GtkSourceBuffer *source_buffer;
#endif                          /* HAVE_GTKSOURCEVIEW */
    GError *error = NULL;
    GtkWidget *menubar;
    GtkWidget *paned;
    const gchar resource_path[] = "/org/desktop/Balsa/sendmsg-window.ui";
    const gchar *current_locale;

    compose_window = g_object_new(BALSA_TYPE_COMPOSE_WINDOW,
                                  "application", balsa_app.application,
                                  NULL);

    window = (GtkWidget *) compose_window;

    /*
     * restore the compose window size
     */
    gtk_window_set_default_size(GTK_WINDOW(window),
                                balsa_app.sw_width,
                                balsa_app.sw_height);
    if (balsa_app.sw_maximized)
        gtk_window_maximize(GTK_WINDOW(window));

    gtk_container_add(GTK_CONTAINER(window), main_box);
    gtk_widget_show(window);

    compose_window->autosave_timeout_id = /* autosave every 5 minutes */
        g_timeout_add_seconds(60 * 5, (GSourceFunc)sw_autosave_timeout_cb,
                              compose_window);

    /* If any compose windows are open when Balsa is closed, we want
     * them also to be closed. */
    g_object_weak_ref(G_OBJECT(balsa_app.main_window),
                      (GWeakNotify) gtk_widget_destroy, compose_window);
    compose_window->have_weak_ref = TRUE;

    /* Set up the GMenu structures */
    menubar = libbalsa_window_get_menu_bar(GTK_APPLICATION_WINDOW(window),
                                           win_entries,
                                           G_N_ELEMENTS(win_entries),
                                           resource_path, &error, compose_window);
    if (error) {
        g_print("%s %s\n", __func__, error->message);
        g_error_free(error);
        return NULL;
    }

#if HAVE_MACOSX_DESKTOP
    libbalsa_macosx_menu(window, GTK_MENU_SHELL(menubar));
#else
    gtk_box_pack_start(GTK_BOX(main_box), menubar);
#endif

    /*
     * Set up the spell-checker language menu
     */
    gtk_container_foreach(GTK_CONTAINER(menubar), sw_menubar_foreach,
                          &compose_window->current_language_menu);
    current_locale = create_lang_menu(compose_window->current_language_menu, compose_window);
    if (current_locale == NULL)
        sw_action_set_enabled(compose_window, "spell-check", FALSE);

    model = balsa_compose_window_get_toolbar_model();
    compose_window->toolbar = balsa_toolbar_new(model, G_ACTION_MAP(window));
    gtk_box_pack_start(GTK_BOX(main_box), compose_window->toolbar);

    compose_window->flow = !balsa_app.wordwrap;
    sw_action_set_enabled(compose_window, "reflow", compose_window->flow);

    compose_window->ident = balsa_app.current_ident;

    sw_action_set_enabled(compose_window, "select-ident",
                          balsa_app.identities->next != NULL);
    compose_window->identities_changed_id =
        g_signal_connect_swapped(balsa_app.main_window, "identities-changed",
                                 (GCallback)compose_window_identities_changed_cb,
                                 compose_window);
#if !HAVE_GTKSOURCEVIEW
    sw_buffer_set_undo(compose_window, TRUE, FALSE);
#endif                          /* HAVE_GTKSOURCEVIEW */

    sw_action_set_active(compose_window, "flowed", compose_window->flow);
    sw_action_set_active(compose_window, "send-html",
                         libbalsa_identity_get_send_mp_alternative(compose_window->ident));
    sw_action_set_active(compose_window, "show-toolbar", balsa_app.show_compose_toolbar);

#ifdef HAVE_GPGME
    compose_window_setup_gpg_ui(compose_window);
#endif

    /* Paned window for the addresses at the top, and the content at the
     * bottom: */
    compose_window->paned = paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_pack_start(GTK_BOX(main_box), paned);

    /* create the top portion with the to, from, etc in it */
    gtk_paned_add1(GTK_PANED(paned), create_info_pane(compose_window));
    compose_window->tree_view = NULL;

    /* create text area for the message */
    gtk_paned_add2(GTK_PANED(paned), create_text_area(compose_window));

    /* set the menus - and language index */
    init_menus(compose_window);

    /* Connect to "text-changed" here, so that we catch the initial text
     * and wrap it... */
    sw_buffer_signals_connect(compose_window);

#if HAVE_GTKSOURCEVIEW
    source_buffer = GTK_SOURCE_BUFFER(gtk_text_view_get_buffer
                                          (GTK_TEXT_VIEW(compose_window->text)));
    gtk_source_buffer_begin_not_undoable_action(source_buffer);
    gtk_source_buffer_end_not_undoable_action(source_buffer);
    sw_action_set_enabled(compose_window, "undo", FALSE);
    sw_action_set_enabled(compose_window, "redo", FALSE);
#else                           /* HAVE_GTKSOURCEVIEW */
    sw_buffer_set_undo(compose_window, FALSE, FALSE);
#endif                          /* HAVE_GTKSOURCEVIEW */

    compose_window->update_config = TRUE;

    compose_window->delete_sig_id =
        g_signal_connect(G_OBJECT(balsa_app.main_window), "close-request",
                         G_CALLBACK(close_request_cb), compose_window);

    setup_headers_from_identity(compose_window, compose_window->ident);

    /* Finish setting up the spell checker */
#if HAVE_GSPELL || HAVE_GTKSPELL
    if (current_locale != NULL)
        sw_action_set_active(compose_window, "spell-check", balsa_app.spell_check_active);

#endif
    set_locale(compose_window, current_locale);

    return compose_window;
}


BalsaComposeWindow *
balsa_compose_window_compose(void)
{
    BalsaComposeWindow *compose_window = balsa_compose_window_new();

    /* set the initial window title */
    compose_window->type = SEND_NORMAL;
    balsa_compose_window_set_title(compose_window);
    if (libbalsa_identity_get_sig_sending(compose_window->ident))
        insert_initial_sig(compose_window);
    compose_window->state = SENDMSG_STATE_CLEAN;
    return compose_window;
}


BalsaComposeWindow *
balsa_compose_window_compose_with_address(const gchar *address)
{
    BalsaComposeWindow *compose_window = balsa_compose_window_compose();
    libbalsa_address_view_add_from_string(compose_window->recipient_view,
                                          "To:", address);
    return compose_window;
}


BalsaComposeWindow *
balsa_compose_window_reply(LibBalsaMailbox *mailbox,
                     guint            msgno,
                     SendType         reply_type)
{
    LibBalsaMessage *message;
    LibBalsaMessageHeaders *headers;
    BalsaComposeWindow *compose_window;
    const gchar *message_id;
    GList *references;
    GList *in_reply_to;

    message = libbalsa_mailbox_get_message(mailbox, msgno);
    g_assert(message != NULL);

    switch (reply_type) {
    case SEND_REPLY_GROUP:
        if (libbalsa_message_get_user_header(message, "list-post") == NULL) {
            g_object_unref(message);
            return NULL;
        }
    case SEND_REPLY:
    case SEND_REPLY_ALL:
        compose_window = balsa_compose_window_new();
        compose_window->type = reply_type;
        break;
    default:
        printf("reply_type: %d\n", reply_type);
        g_assert_not_reached();
    }
    compose_window->parent_message = message;
    set_identity(compose_window, message);

    bsm_prepare_for_setup(message);

    headers = libbalsa_message_get_headers(message);
    set_to(compose_window, headers);

    message_id = libbalsa_message_get_message_id(message);
    if (message_id != NULL)
        set_in_reply_to(compose_window, message_id, headers);
    if (reply_type == SEND_REPLY_ALL)
        set_cc_from_all_recipients(compose_window, headers);

    references  = libbalsa_message_get_references(message);
    in_reply_to = libbalsa_message_get_in_reply_to(message);
    set_references_reply(compose_window, references,
                         in_reply_to != NULL ? in_reply_to->data : NULL,
                         message_id);
    if (balsa_app.autoquote)
        fill_body_from_message(compose_window, message, QUOTE_ALL);
    if (libbalsa_identity_get_sig_whenreply(compose_window->ident))
        insert_initial_sig(compose_window);
    bsm_finish_setup(compose_window, libbalsa_message_get_body_list(message));
    g_idle_add((GSourceFunc) sw_grab_focus_to_text,
               g_object_ref(compose_window->text));
    return compose_window;
}


BalsaComposeWindow *
balsa_compose_window_reply_embedded(LibBalsaMessageBody *part,
                              SendType             reply_type)
{
    BalsaComposeWindow *compose_window = balsa_compose_window_new();
    LibBalsaMessageHeaders *headers;

    g_assert(part);
    g_return_val_if_fail(part->embhdrs, compose_window);

    switch (reply_type) {
    case SEND_REPLY:
    case SEND_REPLY_ALL:
    case SEND_REPLY_GROUP:
        compose_window->type = reply_type;
        break;

    default: printf("reply_type: %d\n", reply_type);
        g_assert_not_reached();
    }
    bsm_prepare_for_setup(g_object_ref(part->message));
    headers = part->embhdrs;
    /* To: */
    set_to(compose_window, headers);

    if (part->embhdrs) {
        const gchar *message_id =
            libbalsa_message_header_get_one(part->embhdrs, "Message-Id");
        const gchar *in_reply_to =
            libbalsa_message_header_get_one(part->embhdrs, "In-Reply-To");
        GList *references =
            libbalsa_message_header_get_all(part->embhdrs, "References");
        if (message_id)
            set_in_reply_to(compose_window, message_id, headers);
        set_references_reply(compose_window, references,
                             in_reply_to, message_id);
        fill_body_from_part(compose_window, part->embhdrs, message_id, references,
                            part->parts, QUOTE_ALL);
        g_list_free_full(references, g_free);
    }

    if (reply_type == SEND_REPLY_ALL)
        set_cc_from_all_recipients(compose_window, part->embhdrs);

    bsm_finish_setup(compose_window, part);
    if (libbalsa_identity_get_sig_whenreply(compose_window->ident))
        insert_initial_sig(compose_window);
    g_idle_add((GSourceFunc) sw_grab_focus_to_text,
               g_object_ref(compose_window->text));
    return compose_window;
}


BalsaComposeWindow *
balsa_compose_window_forward(LibBalsaMailbox *mailbox,
                             guint            msgno,
                             gboolean         attach)
{
    LibBalsaMessage *message;
    BalsaComposeWindow *compose_window;
    LibBalsaMessageBody *body_list;

    message = libbalsa_mailbox_get_message(mailbox, msgno);
    g_assert(message != NULL);

    compose_window       = balsa_compose_window_new();
    compose_window->type = attach ? SEND_FORWARD_ATTACH : SEND_FORWARD_INLINE;
    body_list   = libbalsa_message_get_body_list(message);
    if (attach) {
        if (!attach_message(compose_window, message)) {
            balsa_information_parented(GTK_WINDOW(compose_window),
                                       LIBBALSA_INFORMATION_WARNING,
                                       _("Attaching message failed.\n"
                                         "Possible reason: not enough temporary space"));
        }
        compose_window->state = SENDMSG_STATE_CLEAN;
        compose_window_set_subject_from_body(compose_window, body_list, compose_window->ident);
    } else {
        bsm_prepare_for_setup(message);
        fill_body_from_message(compose_window, message, QUOTE_NOPREFIX);
        bsm_finish_setup(compose_window, body_list);
    }
    if (libbalsa_identity_get_sig_whenforward(compose_window->ident))
        insert_initial_sig(compose_window);
    if (!attach) {
        GtkTextBuffer *buffer =
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(compose_window->text));
        GtkTextIter pos;
        gtk_text_buffer_get_start_iter(buffer, &pos);
        gtk_text_buffer_place_cursor(buffer, &pos);
        gtk_text_buffer_insert_at_cursor(buffer, "\n", 1);
        gtk_text_buffer_get_start_iter(buffer, &pos);
        gtk_text_buffer_place_cursor(buffer, &pos);
    }
    return compose_window;
}


BalsaComposeWindow *
balsa_compose_window_continue(LibBalsaMailbox *mailbox,
                        guint            msgno)
{
    LibBalsaMessage *message;
    BalsaComposeWindow *compose_window;
    LibBalsaMessageHeaders *headers;
    GList *in_reply_to;
    const gchar *postpone_hdr;
    GList *list, *refs = NULL;

    message = libbalsa_mailbox_get_message(mailbox, msgno);
    g_assert(message);

    if ((compose_window = g_object_get_data(G_OBJECT(message),
                                   BALSA_SENDMSG_WINDOW_KEY))) {
        gtk_window_present(GTK_WINDOW(compose_window));
        return NULL;
    }

    compose_window              = balsa_compose_window_new();
    compose_window->is_continue = TRUE;
    bsm_prepare_for_setup(message);
    compose_window->draft_message = message;
    g_object_set_data(G_OBJECT(compose_window->draft_message),
                      BALSA_SENDMSG_WINDOW_KEY, compose_window);
    set_identity(compose_window, message);
    setup_headers_from_message(compose_window, message);

    headers = libbalsa_message_get_headers(message);
    libbalsa_address_view_set_from_list(compose_window->replyto_view,
                                        "Reply To:",
                                        headers->reply_to);
    in_reply_to = libbalsa_message_get_in_reply_to(message);
    if (in_reply_to != NULL)
        compose_window->in_reply_to = g_strconcat("<", in_reply_to->data, ">", NULL);

#ifdef HAVE_GPGME
    if ((postpone_hdr =
             libbalsa_message_get_user_header(message, "X-Balsa-Crypto")))
        compose_window_setup_gpg_ui_by_mode(compose_window, atoi(postpone_hdr));
    postpone_hdr = libbalsa_message_get_user_header(message, "X-Balsa-Att-Pubkey");
    if (postpone_hdr != NULL)
        sw_action_set_active(compose_window, "attpubkey", atoi(postpone_hdr) != 0);

#endif
    if ((postpone_hdr =
             libbalsa_message_get_user_header(message, "X-Balsa-MDN")))
        sw_action_set_active(compose_window, "request-mdn", atoi(postpone_hdr) != 0);
    if ((postpone_hdr =
             libbalsa_message_get_user_header(message, "X-Balsa-DSN")))
        sw_action_set_active(compose_window, "request-dsn", atoi(postpone_hdr) != 0);
    if ((postpone_hdr =
             libbalsa_message_get_user_header(message, "X-Balsa-Lang"))) {
        GtkWidget *langs =
            gtk_menu_item_get_submenu(GTK_MENU_ITEM
                                          (compose_window->current_language_menu));
        GList *children =
            gtk_container_get_children(GTK_CONTAINER(langs));
        set_locale(compose_window, postpone_hdr);
        for (list = children; list; list = list->next) {
            GtkCheckMenuItem *menu_item = list->data;
            const gchar *lang;

            lang = g_object_get_data(G_OBJECT(menu_item),
                                     BALSA_LANGUAGE_MENU_LANG);
            if (strcmp(lang, postpone_hdr) == 0)
                gtk_check_menu_item_set_active(menu_item, TRUE);
        }
        g_list_free(children);
    }
    if ((postpone_hdr =
             libbalsa_message_get_user_header(message, "X-Balsa-Format")))
        sw_action_set_active(compose_window, "flowed", strcmp(postpone_hdr, "Fixed"));
    if ((postpone_hdr =
             libbalsa_message_get_user_header(message, "X-Balsa-MP-Alt")))
        sw_action_set_active(compose_window, "send-html", !strcmp(postpone_hdr, "yes"));
    if ((postpone_hdr =
             libbalsa_message_get_user_header(message, "X-Balsa-Send-Type")))
        compose_window->type = atoi(postpone_hdr);

    list = libbalsa_message_get_references(message);
    while (list != NULL) {
        refs = g_list_prepend(refs, g_strdup(list->data));
        list = list->next;
    }
    compose_window->references = g_list_reverse(refs);

    continue_body(compose_window, message);
    bsm_finish_setup(compose_window, libbalsa_message_get_body_list(message));
    g_idle_add((GSourceFunc) sw_grab_focus_to_text,
               g_object_ref(compose_window->text));
    return compose_window;
}


/*
 * Setter
 */

void
balsa_compose_window_set_quit_on_close(BalsaComposeWindow *compose_window,
                                 gboolean      quit_on_close)
{
    compose_window->quit_on_close = quit_on_close;
}
