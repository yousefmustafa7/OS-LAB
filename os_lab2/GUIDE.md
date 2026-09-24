# Lab 2 — Memory Allocator: Step-by-Step Guide

Deadline: **Friday 2 October 2026, 22:00** (Moodle). Groups of 2.

This guide explains the concepts and the order of work. It does **not** contain the
solution code. You write every line yourselves, because you may be asked to explain
any line in an oral check.

---

## 0. Setup (do this once, both of you)

```bash
sudo apt install build-essential gdb git make   # compiler, debugger, make
uname -m                                         # x86_64 or aarch64 → goes in the report
cat /etc/os-release                              # distro/version → goes in the report
git clone <your repo> && cd OS-LAB/os_lab2/basic
make -B tests/alloc1.test                        # should FAIL right now (nothing implemented)
```

Layout in this repo:

```
os_lab2/
├── lab2.pdf          subject
├── GUIDE.md          this file
├── REPORT.md         report skeleton, filled in as you go
├── basic/            ← work here first (steps 1–4, no alignment, no safety checks)
└── advanced/         ← created later with `cp -r basic advanced` (step 5 + bonus)
```

**Should you use VS Code + the Claude Code extension?** Yes. You need to build and run on
your own Linux machine anyway, and having the editor, terminal and debugger in one place
helps. Install:
- **C/C++** (Microsoft) for autocomplete, go-to-definition, and a graphical gdb.
- **Claude Code** for asking questions such as "why does this segfault?" or "explain
  this line", and for reviewing your diff before you commit. Treat it as a tutor.
  Don't ask it for whole functions, or you won't be able to defend the code.
- **Live Share** (optional) for pair programming on the hard parts.

### Git workflow for two people
- Each person works on their own branch (`yousef/fast-pool`, `partner/std-pool`) and merges
  into the shared branch when the tests for their part pass.
- The two of you mostly edit **different files**
  (`mem_alloc_fast_pool.c` vs `mem_alloc_standard_pool.c`), so merge conflicts should be rare.
  `mem_alloc.c` is shared, so tell each other before you edit it.
- Commit small and often, with messages like `fast pool: LIFO alloc + free, alloc1-4 pass`.

---

## 1. The mental model (read this before writing any code)

`my_mmap(65536)` gives you 64 KiB of raw bytes, and that's your whole heap for one pool.
The allocator has no other memory to work with. **Everything, including the linked-list
pointers, lives inside those 64 KiB.** That's the key idea of the lab:

> A free block has no user data in it, so you use its own bytes to store the
> "next free block" pointer. When you hand the block to the user, the user overwrites
> those bytes. That's fine, because an allocated block no longer needs to be in the list.

This is the same trick used by glibc `malloc` (`fd`/`bk` pointers in free chunks).
Heap exploitation also relies on it. **Use-after-free** and **double-free** attacks
(tcache poisoning, the "House of …" techniques) work by corrupting exactly these in-band
pointers. So this lab is also groundwork for exploit dev and memory forensics. If
you ever carve heap structures out of a RAM dump with Volatility, you'll recognise the layout.

### Pools
| Pool | Request size | Block size | Free-list type | Split/coalesce |
|---|---|---|---|---|
| 0 | 1–64 (and 0) | 64 fixed | singly linked, LIFO | never |
| 1 | 65–256 | 256 fixed | singly linked, LIFO | never |
| 2 | 257–1024 | 1024 fixed | singly linked, LIFO | never |
| 3 | > 1024 | variable | **doubly** linked, **sorted by address** | yes |

The dispatcher in `mem_alloc.c` already picks the pool for you. You implement the pool
functions, `print_mem_state()`, and later `run_at_exit()`.

### How to read the traces
Traces print **offsets from the pool start**, not raw addresses:
```
ALLOC at : 64 (16 byte(s)) -- pool 0     → payload starts at pool_start + 64
```
If your trace matches the simulator (`bin/mem_shell_sim`) line for line, the test passes.

---

## 2. Step 1: Fast pools (`mem_alloc_fast_pool.c`)

### Answer these first (they go in the report, in your own words)
1. Where is the metadata stored? *(Hint: see the mental model above.)*
2. Minimum size of a free block? Of an allocated block? *(Hint: what does a free block need to hold?
   `sizeof(mem_fast_free_block_t)` = ?)*
3. Do you need a list of allocated blocks?
4. What metadata does a free block need? An allocated block? *(Hint: how does
   `mem_get_allocated_block_size_fast_pool` already work without reading the block at all?)*
5. Where does a freed block go in the free list? *(Hint: LIFO.)*

### What each function must do
**`init_fast_pool(p, size, min, max)`**
- Get the region with `my_mmap(size)`.
- Set `p->start_addr`, `p->end_addr`. Careful: `find_pool_from_address` uses
  `addr <= end_addr`, so `end_addr` must be the **last valid byte**, not one past it.
- Block size = `max_request_size` (64 / 256 / 1024).
- Split the region into blocks and **chain them all** into the free list, then set `p->first_free`.
- Which order should the chain be in? Look at the expected trace for `alloc1`: the first
  allocation returns offset **0**, then **64**, then **128**. Work out what order the
  list must start in to produce that.

**`mem_alloc_fast_pool(pool, size)`**: pop the head of the list. Return `NULL` if the list is
empty (the dispatcher then prints the error and calls `exit(0)`).

**`mem_free_fast_pool(pool, b)`**: push `b` onto the head of the list.

### C pointer arithmetic you will need
```c
char *base = (char *)p->start_addr;          // char* so that +1 == +1 byte
mem_fast_free_block_t *blk = (mem_fast_free_block_t *)(base + i * block_size);
blk->next = ...;                             // writes 8 bytes INTO the heap
```
Arithmetic on `void *` is a GCC extension (`-Wpointer-arith` warns about it). Cast to
`char *` before adding byte offsets.

### Pitfall: `memory_alloc(0)`
The subject says `memory_alloc(0)` must return a valid pointer. The simulator puts it in
**pool 0**. But `find_pool_from_block_size(0)` hits `assert(res >= 0)`, because pool 0's
`min_req_size` is 1. You have to handle that case in `mem_alloc.c`. Decide how, and
explain your choice in the report.

### Validate
```bash
make -B tests/alloc1.test   # pool 0 allocs
make -B tests/alloc2.test   # 3 fast pools
make -B tests/alloc3.test   # pool 0 alloc/free (LIFO reuse!)
make -B tests/alloc4.test   # all fast pools alloc/free
```
In `alloc3`, block 0 is freed and the next allocation gets offset 0 back. That shows the LIFO
behaviour.

Also try a tiny pool to trigger the "pool full" path:
`make -B MEM_POOL_0_SIZE=256 tests/alloc1.test`. Only 4 blocks fit, so the 5th allocation must fail cleanly.

---

## 3. Step 3: `print_mem_state()` (`mem_alloc.c`); start this right after Step 1

Goal: one symbol per byte, e.g. `.` = free, `X` = allocated.

Idea for fast pools: mark everything as allocated, then **walk the free list** and mark
those blocks free. Printing 65536 characters per pool is unreadable, so improve it:
- one char per block for fast pools, or one char per N bytes (explain the scale in the report);
- for pool 3, also show headers/footers differently (e.g. `H`/`F`) — this is very useful
  for debugging coalescing.

`mem_alloc.c` can't see the block layout directly. A clean design is to add a
`print_*_pool_state(mem_pool_t *)` function in each pool file, declare it in the pool's `.h`,
and call it from `print_mem_state()`.

Test it interactively:
```bash
make -B mem_shell && ./bin/mem_shell
a10
a100
p
f1
p
q
```

---

## 4. Step 2: Standard pool, first fit (`mem_alloc_standard_pool.c`)

This is the hardest step. Draw diagrams on paper before you write code.

### Block layout (Figure 1 in the PDF)
```
Allocated:  [ H:1|size ][ ........ payload (size bytes) ........ ][ F:1|size ]
Free:       [ H:0|size ][ prev* ][ next* ][ ...old payload... ][ F:0|size ]
              8 bytes                                              8 bytes
```
- `size` stored in H/F = **payload only**, header and footer excluded.
- Bit 63 = used flag. Use the provided helpers in `mem_alloc_standard_pool_types.c`
  (`set_block_used`, `get_block_size`…). Don't redo the bit manipulation yourself.

### Check the layout against the expected trace (`alloc6`)
```
ALLOC at : 8    (2048)   → header at 0, payload at 8
ALLOC at : 2072 (4096)   → 8 + 2048 (payload) + 8 (footer) + 8 (next header) = 2072
ALLOC at : 6184 (2048)   → 2072 + 4096 + 8 + 8 = 6184
```
If you can explain every number there, you understand the layout. Also note that sizes are
**not** rounded up in the basic version (`alloc5`: 1233 bytes at 6184, next block at 7433).

### Write small helper functions first (these make the code clearer and easier to grade)
Suggested helpers (your own names, `static` in the `.c`):
- header → payload address, payload → header address
- header → footer address (uses the size)
- next block in memory (right neighbour), previous block in memory (left neighbour,
  **found by reading its footer**, which is why footers exist)
- "is this address still inside the pool?" (for the first and last blocks)
- free-list insert (sorted by address), free-list remove (unlink from a doubly linked list)

Test each helper on its own with gdb before building on top of it.

### The report questions (answer them before coding)
1. Minimum free block size? Minimum allocated block size? *(Count header + prev + next + footer.)*
2. Do you need an allocated-block list?
3. Which address do you return to the user? *(Header or payload?)*
4. Which address does the user pass to `free`? How do you get back to the header?
5. What happens on free? Where does the block go in the list? *(Sorted by address. Hint: you
   don't always have to walk the whole list; think about the left/right neighbours.)*
6. The chosen free block is bigger than the request. What happens to the rest? *(Split it.)*
7. What if the rest is **too small to become a valid free block**? *(Hint: Q1. Give the whole block
   to the user instead, and `get_allocated_block_size` must then report the real size.)*

### Algorithms (in words)
**init**: `my_mmap` the region and make **one** big free block covering it:
payload = `size − header − footer`. The list contains just that one block.

**alloc (first fit)**: walk the list from the head, take the first block with `size(b) >= req`.
Then either split it (the first part is allocated, the remainder becomes a new free block that takes
`b`'s place in the list, which keeps address order) or allocate it whole. Update header **and** footer.

**free + immediate coalescing**: from the payload, get the header and mark the block free. Then look at the
left and right neighbours in memory. There are 4 cases:

| left | right | action |
|---|---|---|
| used | used | insert block into the list (sorted) |
| used | free | merge with right; merged block takes right's list position |
| free | used | merge into left; left's list position is unchanged |
| free | free | merge all three; remove right from the list |

The merged size also absorbs the header/footer bytes that sat between the blocks
(`+ 16` per boundary removed). Draw each case.

**get_allocated_block_size**: read the header in front of the payload.

### Validate
`alloc5` → `alloc9`. `alloc7`/`alloc8` exercise all 4 coalescing cases. If a trace diverges,
run the scenario interactively with `p` after each line, and use gdb (see `basic/gdb_README.md`):
```bash
gdb ./bin/mem_shell
(gdb) run < tests/alloc8.in
(gdb) break mem_free_standard_pool
```

---

## 5. Step 4: Best fit

The code branches on `std_pool_policy` (already defined from the Makefile). Best fit walks the
**whole** list and keeps the block with the smallest `size(b) − req`, breaking ties by first
occurrence. Everything after the choice (split, list update) is the same, so factor it
into a shared helper and you'll write it only once.
```bash
make -B STDPOOL_POLICY=BF tests/alloc9.test
```
Design **your own** test where FF and BF give different results (e.g. free a big block
early and a snug-fitting block later, then request the snug size). Put it in
`basic/tests/` and describe it in the report. The subject explicitly rewards new tests.

---

## 6. Step 5: Alignment (in `advanced/` only)

```bash
cp -r basic advanced     # do this ONLY once basic passes everything; commit first
```
Build with `make -B MEM_ALIGN_CONSTRAINT=8 tests/alloc5.test`.

- Rounding formula you'll need: round `x` up to a multiple of `A` (a power of 2).
  Work out the bit-mask version yourself. It's a classic, and you should be able to derive
  it on a whiteboard.
- The constraint applies to **payloads and metadata**. In pool 3, if the header is at an
  aligned address and is 8 bytes, then payload size is what must be rounded so that the
  **footer and the next header** stay aligned.
- Check against the simulator: with `MEM_ALIGN_CONSTRAINT=8`, `alloc5` gives
  `1233 at 6184` then the next block at **7440** (not 7433). Explain that number and you've
  got it.
- Fast pools: 64/256/1024 are already multiples of 2…64, so what changes there, if anything?
- Test all of 2, 4, 8, 16, 32, 64 and list in the report which values pass.
  For values > 8, think about the 8-byte header at the start of the pool.

---

## 7. Bonus: Safety checks (in `advanced/`)

Pick at least one. From easiest to hardest:
1. **Memory leak on exit**: keep a counter (or bytes total) of live allocations and
   print a warning in `run_at_exit()`. Test: a scenario that allocates without freeing.
2. **Bad free**: address not in any pool (`find_pool_from_address` returns −1), address not on a
   block boundary (fast pool: `(addr − start) % block_size != 0`), block already free
   (**double free**: header flag = 0 in pool 3). Exit with an error message.
3. **Metadata corruption**: header ≠ footer, size larger than the pool, list pointers
   outside the pool, `b->next->prev != b`. The last one is exactly the "safe unlinking"
   check glibc added against the classic `unlink` exploit, so cite that in the report.

For each check, write a small C test program (or `.in` scenario) that triggers it. In the report,
say which bug classes you handle and how.

---

## 8. Splitting the work (8 days)

| When | Person A | Person B |
|---|---|---|
| Thu 24 – Fri 25 | Step 1 fast pools → `alloc1–4` pass | Read §4, draw diagrams, write std-pool helpers + `init` |
| Sat 26 – Sun 27 | Step 3 display for fast pools, then for pool 3 | Step 2 alloc (FF + split) → `alloc5` passes |
| Mon 28 | **Together**: free + coalescing (4 cases) → `alloc6–9` pass | ← same |
| Tue 29 | Step 4 best fit + custom FF/BF test | `cp -r basic advanced`, Step 5 alignment |
| Wed 30 | Bonus: leak check + bad-free check | Finish alignment 2…64 |
| Thu 1 Oct | **Together**: report, each reviews the other's code line by line | ← same |
| Fri 2 Oct | Buffer, clean, archive, submit before 22:00 | |

Swap roles on review. The oral check can target either of you on any line.

---

## 9. Submission checklist

```bash
cd os_lab2
(cd basic && make clean) && (cd advanced && make clean)
rm -f basic/bin/mem_shell* advanced/bin/mem_shell*
mkdir Name1--Name2--lab2
cp -r REPORT.md basic advanced Name1--Name2--lab2/
tar czf Name1--Name2--lab2.tar.gz Name1--Name2--lab2
tar tzf Name1--Name2--lab2.tar.gz | less    # check that it has no .o, binaries, or .out files
```
Report must be `.md`, `.txt` or `.pdf`. Use your **last names** in the archive name.

## 10. Common beginner bugs
- Pointer arithmetic on the wrong type (`blk + 64` on a struct pointer moves 64 × sizeof(struct)).
- Forgetting to update the **footer** too.
- `end_addr` off by one, so the free of the last block goes to the wrong pool.
- `printf` inside the allocator in the `test_ls` build: `printf` itself may call `malloc`.
- Forgetting `-B`, so you're testing an old binary.
- The `-e` shown before "Test … Passed/FAILED" is just `/bin/sh` not understanding
  `echo -e`. It's harmless.
