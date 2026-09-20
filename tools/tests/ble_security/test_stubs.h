/* Host-only fakes. These are not used in any firmware target. */
#ifndef BLE_SECURITY_TEST_STUBS_H
#define BLE_SECURITY_TEST_STUBS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#define OPENPENDANT_LONG_CONTROL 1
typedef intptr_t ssize_t;
typedef ssize_t (*settings_read_cb)(void*,void*,size_t);
int settings_load_subtree_direct(const char*,int (*)(const char*,size_t,settings_read_cb,void*,void*),void*);
#define CONFIG_BT_SMP_SC_ONLY 1
#define CONFIG_BT_SMP_APP_PAIRING_ACCEPT 1
#define CONFIG_BT_SMP_ENFORCE_MITM 1
#define CONFIG_BT_BONDING_REQUIRED 1
#define CONFIG_BT_SETTINGS 1
#define CONFIG_SETTINGS_NVS 1
#define CONFIG_BT_MAX_PAIRED 1
#define CONFIG_BT_MAX_CONN 1
#define CONFIG_BT_SMP_MIN_ENC_KEY_SIZE 16
#define CONFIG_BT_KEYS_OVERWRITE_OLDEST 0
#define CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE 0
#define CONFIG_BT_USE_DEBUG_KEYS 0
#define CONFIG_BT_STORE_DEBUG_KEYS 0
#define CONFIG_BT_LOG_SNIFFER_INFO 0
#define CONFIG_BT_FIXED_PASSKEY 0
#define CONFIG_BT_APP_PASSKEY 0
#define CONFIG_SETTINGS_NVS_SECTOR_SIZE_MULT 1
#define CONFIG_SETTINGS_NVS_SECTOR_COUNT 8
#define IS_ENABLED(x) (x)
#define BUILD_ASSERT(x) _Static_assert(x, #x)
#define DT_CHOSEN(x) 1
#define DT_NODELABEL(x) TEST_NODE_##x
#define TEST_NODE_storage_partition 1
#define TEST_NODE_flash0 2
#define DT_PARENT(x) 3
#define DT_SAME_NODE(a,b) ((a)==(b))
#define DT_REG_ADDR(x) ((x)==1 ? 0xf8000 : 0)
#define DT_REG_SIZE(x) ((x)==1 ? 0x8000 : 0x100000)
#define DT_MEM_FROM_FIXED_PARTITION(x) 2
#define DT_MTD_FROM_FIXED_PARTITION(x) 3
#define DT_PROP(x,p) TEST_PROP_##p
#define TEST_PROP_erase_block_size 4096
#define TEST_PROP_write_block_size 4
#define ARG_UNUSED(x) (void)(x)
#define MAX(a,b) ((a)>(b)?(a):(b))
#define K_MSEC(ms) (ms)
typedef int atomic_t;
#define ATOMIC_INIT(x) (x)
static inline int atomic_get(atomic_t *x) { return *x; }
static inline void atomic_set(atomic_t *x, int v) { *x=v; }
static inline bool atomic_cas(atomic_t *x,int a,int b) { if(*x!=a)return false; *x=b;return true; }
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
struct k_work { int unused; };
struct k_work_delayable { void (*handler)(struct k_work *); int64_t due; bool scheduled; };
#define K_WORK_DELAYABLE_DEFINE(name,fn) struct k_work_delayable name={fn,0,false}
k_spinlock_key_t k_spin_lock(struct k_spinlock *lock);
void k_spin_unlock(struct k_spinlock *lock,k_spinlock_key_t key);
int64_t k_uptime_get(void);
int k_work_reschedule(struct k_work_delayable *work,int64_t delay);
int k_work_cancel_delayable(struct k_work_delayable *work);
#define BT_ID_DEFAULT 0
#define BT_CONN_TYPE_LE 1
#define BT_CONN_ROLE_PERIPHERAL 1
#define BT_CONN_STATE_CONNECTED 2
#define BT_SECURITY_FLAG_SC 1
#define BT_HCI_ERR_AUTH_FAIL 5
typedef int bt_security_t;
enum { BT_SECURITY_L1=1,BT_SECURITY_L2=2,BT_SECURITY_L3=3,BT_SECURITY_L4=4 };
enum bt_security_err { BT_SECURITY_ERR_SUCCESS, BT_SECURITY_ERR_AUTH_REQUIREMENT,
 BT_SECURITY_ERR_PAIR_NOT_ALLOWED, BT_SECURITY_ERR_AUTH_FAIL };
typedef struct { int value; } bt_addr_le_t;
struct bt_conn_info { int type, role, id, state; struct { bt_security_t level; uint8_t enc_key_size; int flags; } security; };
struct bt_conn { struct bt_conn_info info; bt_addr_le_t address; int refs,disconnected,cancelled,set_security; };
struct bt_bond_info { int unused; };
struct bt_conn_pairing_feat { uint8_t io_capability,oob_data_flag,auth_req,max_enc_key_size,init_key_dist,resp_key_dist; };
struct bt_conn_auth_cb { enum bt_security_err (*pairing_accept)(struct bt_conn*,const struct bt_conn_pairing_feat*const); void (*passkey_display)(struct bt_conn*,unsigned int); void (*cancel)(struct bt_conn*); };
struct bt_conn_auth_info_cb { void (*pairing_complete)(struct bt_conn*,bool); void (*pairing_failed)(struct bt_conn*,enum bt_security_err); };
struct bt_conn_cb { void (*connected)(struct bt_conn*,uint8_t); void (*disconnected)(struct bt_conn*,uint8_t); void (*security_changed)(struct bt_conn*,bt_security_t,enum bt_security_err); };
#define BT_CONN_CB_DEFINE(name) struct bt_conn_cb name
int bt_conn_get_info(struct bt_conn*,struct bt_conn_info*);
const bt_addr_le_t *bt_conn_get_dst(struct bt_conn*);
bool bt_le_bond_exists(int,const bt_addr_le_t*);
void bt_foreach_bond(int,void (*)(const struct bt_bond_info*,void*),void*);
struct bt_conn *bt_conn_ref(struct bt_conn*);
void bt_conn_unref(struct bt_conn*);
int bt_conn_auth_cancel(struct bt_conn*);
int bt_conn_disconnect(struct bt_conn*,int);
int bt_conn_set_security(struct bt_conn*,bt_security_t);
int bt_conn_auth_cb_register(const struct bt_conn_auth_cb*);
int bt_conn_auth_info_cb_register(struct bt_conn_auth_info_cb*);
int settings_load(void);
int bt_unpair(uint8_t,const bt_addr_le_t*);
struct shell { int id; };
const struct shell *shell_backend_uart_get_ptr(void);
void shell_print(const struct shell*,const char*,...);
#define SHELL_CMD_ARG(a,b,c,d,e,f) 0
#define SHELL_SUBCMD_SET_END 0
#define SHELL_STATIC_SUBCMD_SET_CREATE(name,...) int name[]={__VA_ARGS__}
#define SHELL_CMD_REGISTER(a,b,c,d)
int test_strcmp(const char*,const char*);
#define strcmp test_strcmp
#endif
