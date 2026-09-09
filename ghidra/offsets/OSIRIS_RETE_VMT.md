# Osiris RETE node vtables (macOS arm64)

Game build 4.1.1.7398727, `libOsiris.dylib`. Needed to hook database inserts /
deletes, PROC calls and QRY calls the way upstream's
`Osiris/Shared/NodeHooks.cpp` does on Windows.

## Do not reuse Norbyte's slot numbers here

Upstream's `NodeVMT` struct is the MSVC layout and its slots do **not** line up
with this build. The port previously carried:

```c
#define NODEVMT_IS_DATA_NODE  0x18
#define NODEVMT_IS_VALID      0x20
#define NODEVMT_INSERT_TUPLE  0x50   /* WRONG on macOS */
#define NODEVMT_DELETE_TUPLE  0x60
```

Dumping a live node's vtable and resolving every entry against
`nm -n libOsiris.dylib` gives:

Full map, dumped live with `dladdr` (`CReteFact` and `CReteEvent` are identical
in every slot below except the class-specific ones marked *):

| Offset from vptr | Symbol |
|---|---|
| `+0x00` / `+0x08` | `~CReteFact()` / deleting dtor * |
| `+0x10` / `+0x18` | `CReteNode::pDBase()` (non-const / const) |
| `+0x20` | `CReteStartNode::IsStartNode() const` |
| `+0x28` | `CReteFact::Valid(COsiVarIDValuePairList*, PCReteAdaptor)` * |
| `+0x30` / `+0x38` | `ValidTestAlwaysFails` / `TreeValidTestAlwaysFails` |
| `+0x40` | `CReteNode::pParent()` |
| `+0x48` | `CReteStartNode::SetChild(CReteConnection)` |
| `+0x50` | `CReteStartNode::Adaptor(TReteEntryPoint) const` |
| `+0x58` | `CReteStartNode::Add(COsiVarIDValuePairList*, PCReteNode, TReteEntryPoint)` |
| `+0x60` | `CReteStartNode::Del(COsiVarIDValuePairList*, PCReteNode, TReteEntryPoint)` |
| **`+0x68`** | **`CReteStartNode::Add(COsipParameterList*)`** — upstream's `InsertTuple` |
| **`+0x70`** | **`CReteStartNode::Del(COsipParameterList*)`** — upstream's `DeleteTuple` |
| `+0x78` | `CReteStartNode::ForwardAddToken(CTuple const&)` |
| `+0x80` | `CReteStartNode::ForwardDelToken(CTuple const&)` |
| `+0x88`..`+0x98` | `pTokenGenerator` / `GeneratorConnection` / `DistanceTokenGenerator` |
| `+0xa0` | `CReteFact::Write(COsiSmartBuf*) const` * |
| `+0xa8` / `+0xb0` | `_Log(char*)` / `_LogParents(char*)` |
| `+0xb8` | `CReteNode::SetLinenoOfThen(unsigned int)` |
| `+0xc8`+ | next class's vtable (`_ZTI...` typeinfo boundary) |

**The hook points for Lua listeners are `+0x68` and `+0x70`** — those are what
upstream wraps as `InsertTuple`/`DeleteTuple` in `NodeHooks.cpp`, on both
`CReteFact` (databases) and `CReteEvent` (procs). Note the parameter is a
`COsipParameterList*`, not upstream's `TuplePtrLL*`.

### Firing a proc needs `Add` (+0x68), not `ForwardAddToken` (+0x78)

`osi_node_insert_tuple` builds a `CTuple`, calls `CReteDBase::insert` when the
node has a database, then calls `ForwardAddToken`. That is correct for a
database — `insert` does the real work and the stored row is the visible effect
— but it does **not** run a proc. Upstream's `OsiInsert` calls `InsertTuple`
(`Add(COsipParameterList*)`), which performs the validation, adaptor selection
and entry-point bookkeeping that `ForwardAddToken` assumes has already happened.
Observed 2026-09-08: `Osi.MakePlayer(char)` routed to the `MakePlayer/1` story
proc, logged `tuple inserted (1 args, no database — proc/event)`, raised no
error, and had no effect. Wiring procs to `+0x68` requires the
`COsipParameterList` layout, which is not yet RE'd.

So the insert/delete entry points are **`Add` (+0x58)** and **`Del` (+0x60)** —
`+0x50` is `Adaptor`, and patching it in the belief that it was `InsertTuple`
would have replaced an unrelated virtual with no immediate symptom.

Two further corrections that fell out of the same dump:

- `+0x18` is **`pDBase()`**, which returns a `CReteDBase*`, not a bool. It is
  usable as "is this a data node" (databases have one, procs do not) — that is
  the same distinction upstream routes `FunctionType::Database` on — but calling
  it through a `bool (*)(void*)` cast reads only the low byte of `x0`, so a
  `CReteDBase` at an address ending in `0x00` would read as `false`. Call it as
  `void *(*)(void*)` and test for NULL (`osi_node_pdbase` in `main.c`).
- The `Add`/`Del` parameter is a `COsiVarIDValuePairList*`, not upstream's
  `TuplePtrLL*`; the tuple layout must be read from this build before a handler
  can marshal arguments to Lua.

## Which class backs which symbol

From the live story (`node probe` lines in the BG3SE log):

| Osiris type | Node class | Example |
|---|---|---|
| Database (type 4) | `CReteFact` | `DB_Players` nodeId=2, cols=1 |
| Proc (type 5) | `CReteEvent` | `PROC_GLO_PartyMembers_Add` nodeId=13382 |

`CReteFact` and `CReteEvent` both inherit `Add`/`Del`/`Adaptor` from
`CReteStartNode` without overriding them — the two classes' vtables hold the
same pointers in those slots.

## Vtable addresses

`osi_report_node_classes()` in `src/injector/main.c` carries the symbol offsets
for all 15 `CRete*` classes. The pointer stored in an object is the vtable
symbol **+ 0x10** (Itanium ABI: offset-to-top and typeinfo precede the first
virtual).

## Reading them

Only from a live process. On disk these slots are dyld chained-fixup entries,
not addresses — dumping the raw bytes yields nonsense. `osi_report_node_classes()`
dumps each distinct node vtable once at story load, naming every entry with
`dladdr`; read it from the BG3SE log.
