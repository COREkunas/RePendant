#ifndef RB_TEST_CONN_H
#define RB_TEST_CONN_H
struct bt_conn { unsigned id,refs,authorized,mtu,subscribed,connected; };
struct bt_conn *bt_conn_ref(struct bt_conn *);
void bt_conn_unref(struct bt_conn *);
#endif
