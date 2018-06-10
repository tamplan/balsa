/* -*-mode:c; c-style:k&r; c-basic-offset:4; -*- */
/* Balsa E-Mail Client
 * Copyright (C) 1997-2016 Stuart Parmenter and others,
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
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __BALSA_SENDMSG_H__
#define __BALSA_SENDMSG_H__

#ifndef BALSA_VERSION
#   error "Include config.h before this file."
#endif

#include "libbalsa.h"
#include "address-view.h"
#include "toolbar-factory.h"

G_BEGIN_DECLS

typedef enum {
    SEND_NORMAL,               /* initialized by Compose */
    SEND_REPLY,                /* by Reply               */
    SEND_REPLY_ALL,            /* by Reply All           */
    SEND_REPLY_GROUP,          /* by Reply to Group      */
    SEND_FORWARD_ATTACH,       /* by Forward attached    */
    SEND_FORWARD_INLINE,       /* by Forward inline      */
    SEND_CONTINUE              /* by Continue postponed  */
} SendType;

#define VIEW_MENU_LENGTH 5

#define BALSA_TYPE_COMPOSE_WINDOW balsa_compose_window_get_type()

G_DECLARE_FINAL_TYPE(BalsaComposeWindow,
                     balsa_compose_window,
                     BALSA,
                     COMPOSE_WINDOW,
                     GtkApplicationWindow);

BalsaComposeWindow *balsa_compose_window_compose(void);
BalsaComposeWindow *balsa_compose_window_compose_with_address(const gchar *
                                                  address);
BalsaComposeWindow *balsa_compose_window_reply(LibBalsaMailbox *mailbox,
                                   guint    msgno,
                                   SendType rt);
BalsaComposeWindow *balsa_compose_window_reply_embedded(LibBalsaMessageBody *part,
                                            SendType             reply_type);

BalsaComposeWindow *balsa_compose_window_forward(LibBalsaMailbox *mailbox,
                                     guint    msgno,
                                     gboolean attach);
BalsaComposeWindow *balsa_compose_window_continue(LibBalsaMailbox *mailbox,
                                      guint msgno);

void balsa_compose_window_set_field(BalsaComposeWindow *bsmsg,
                              const gchar  *key,
                              const gchar  *val);

gboolean add_attachment(BalsaComposeWindow *bsmsg,
                        const gchar  *filename,
                        gboolean      is_a_tmp_file,
                        const gchar  *forced_mime_type);

typedef void (*field_setter)(BalsaComposeWindow *d,
                             const gchar *key,
                             const gchar *value);

void balsa_compose_window_process_url(const char  *url,
                                field_setter func,
                                void        *data);
BalsaComposeWindow *balsa_compose_window_new_from_list(LibBalsaMailbox *mailbox,
                                           GArray          *selected,
                                           SendType         type);
BalsaToolbarModel *balsa_compose_window_get_toolbar_model(void);
void               balsa_compose_window_add_action_entries(GActionMap *action_map);

/*
 * Setter
 */

void balsa_compose_window_set_quit_on_close(BalsaComposeWindow *bsmsg,
                                      gboolean      quit_on_close);

G_END_DECLS

#endif                          /* __BALSA_SENDMSG_H__ */
