#ifndef OPENPENDANT_CRYPTO_COMMANDS_H
#define OPENPENDANT_CRYPTO_COMMANDS_H

/* Local UART-shell PUBLIC synthetic crypto test, once per boot; dormant until
 * `cryptodiag run public-confirm`. Init starts only an idle supervisor queue;
 * the worker waits on a semaphore. No audio/NAND/owner-key operation exists.
 *
 * Fixed decimal metadata protocol (one line each; no delivery ACK implied):
 * CRYPTO_ACCEPTED public_only=1 timeout_ms=20000
 * CRYPTO_REFUSED rc=<signed errno>
 * CRYPTO_STATUS state=<0..5> error=<signed errno> core_error=<signed>
 *   completed=<0..2> owner_faulted=<0|1> elapsed_ms=<u32>
 *   stack_unused=<u32> stack_valid=<0|1> reservation_held=<0|1>
 *   timeout_ms=20000 public_only=1
 * State: 0 IDLE, 1 RUNNING, 2 FINISHING, 3 DONE, 4 FAILED, 5 EXPIRED.
 * Terminal fields are published together; nonterminal fields are zero except
 * state/reservation. FAILED never exports even a partial container.
 * `cryptodiag result` only after DONE prints, in order:
 * CRYPTO_RESULT containers=2 bytes_each=238 public_only=1
 * CRYPTO_DATA index=<0|1> offset=<0,32,...,224> bytes=<32|14> hex=<lowercase>
 *   (exactly eight lines per container, at most 32 bytes per line)
 * CRYPTO_RESULT_END containers=2 bytes=476 public_only=1
 * Refused/unknown arguments are never accepted as work. Shell transport errors
 * cannot be detected by the void shell_print API: a host must validate framing.
 *
 * A dedicated priority-4 preemptible timeout queue can preempt priority-14
 * crypto. It cannot prove a hard deadline against IRQ masking, scheduler
 * starvation or fatal kernel/provider faults. At 20 s timeout/late completion
 * cold-reboots with exclusion retained, without logging. FINISHING joins the
 * bounded timeout callback before stack inspection/publication/release; the
 * synchronous cancellation relies on the kernel and nonblocking callback.
 * Worker stack 16384 bytes, supervisor 2048. A missing stack measurement or
 * <1024 unused worker bytes fails closed, retaining exclusion until reboot.
 * Owner-clean ordinary errors release exclusion; an owner fault never does.
 * No reset/retry API; firmware caller must invoke init exactly once.
 */
int crypto_commands_init(void);

#endif
