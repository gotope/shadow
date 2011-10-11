/**
 * The Shadow Simulator
 *
 * Copyright (c) 2010-2011 Rob Jansen <jansen@cs.umn.edu>
 *
 * This file is part of Shadow.
 *
 * Shadow is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shadow is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Shadow.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SHD_NODE_H_
#define SHD_NODE_H_

#include "shadow.h"

#include <netinet/in.h>

typedef struct _Node Node;

struct _Node {
	/* asynchronous event priority queue. other nodes may push to this queue. */
	GAsyncQueue* event_mailbox;

	/* general node lock. nothing that belongs to the node should be touched
	 * unless holding this lock.
	 */
	GMutex* node_lock;

	/* a simple priority queue holding events currently being executed.
	 * events are place in this queue before handing the node off to a
	 * worker and should not be modified by other nodes. */
	GQueue* event_priority_queue;

	GQuark id;
	Network* network;
	Application* application;
	PluginState* state;

	MAGIC_DECLARE;
};

Node* node_new(GQuark id, Network* network, Application* application, in_addr_t ipAddress, GString* hostname, guint32 KBps_down, guint32 KBps_up, guint64 cpu_speed_Bps);
void node_free(gpointer data);

void node_lock(Node* node);
void node_unlock(Node* node);

void node_pushMail(Node* node, Event* event);
Event* node_popMail(Node* node);
void node_pushTask(Node* node, Event* event);
Event* node_popTask(Node* node);
guint node_getNumTasks(Node* node);

gint node_compare(gconstpointer a, gconstpointer b, gpointer user_data);
gboolean node_equal(Node* a, Node* b);

#endif /* SHD_NODE_H_ */
