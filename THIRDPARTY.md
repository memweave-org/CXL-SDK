# Third-Party Notices

CXL-SDK incorporates and adapts third-party research software. Those components
remain subject to their own copyright notices and license terms; the repository's
MIT license does not replace them.

The following inventory covers principal bundled components in the current
tree. The root MIT license applies only to original CXL-SDK contributions; it
does not replace the terms or copyright notices of these components. When
adding or updating third-party code, update this file in the same pull request.

| Component | Location | License or notice |
| --- | --- | --- |
| STAMP | `apps/stamp/` | [`apps/stamp/LICENSE`](apps/stamp/LICENSE) and component notices |
| BwTree | `ds/BwTree/` | [`ds/BwTree/LICENSE`](ds/BwTree/LICENSE) |
| Masstree adapter | `ds/Masstree/` | Derived from RECIPE P-Masstree; source and license attribution require verification before redistribution. See [RECIPE upstream](https://github.com/utsaslab/RECIPE) and [original Masstree license](https://github.com/kohler/masstree-beta/blob/master/LICENSE). |
| CLHT | `ds/CLHT/` | [`ds/CLHT/LICENSE`](ds/CLHT/LICENSE) |
| HOT | `ds/HOT/` | [`ds/HOT/LICENSE`](ds/HOT/LICENSE) |
| RadixART | `ds/RadixART/` | [`ds/RadixART/LICENSE`](ds/RadixART/LICENSE) |
| YCSB-C | `tests/YCSB-C/` | [`tests/YCSB-C/LICENSE`](tests/YCSB-C/LICENSE) (Apache-2.0) |
| SwissTM | `TXMem/swisstm/` | [`TXMem/swisstm/LICENSE`](TXMem/swisstm/LICENSE) |
| TinySTM / VELOX | `TXMem/tinystm/` | [`TXMem/tinystm/LICENSE.VELOX`](TXMem/tinystm/LICENSE.VELOX) and bundled notices |
| TL2 | `TXMem/tl2/` | [`TXMem/tl2/LICENSE`](TXMem/tl2/LICENSE) |
| CLHT bundled with lsmalloc | `allocator/lsmalloc/3rdparty/clht/` | [`allocator/lsmalloc/3rdparty/clht/LICENSE`](allocator/lsmalloc/3rdparty/clht/LICENSE) (MIT) |

Some components contain nested dependencies with additional `LICENSE`, `COPYING`,
`NOTICE`, `AUTHORS`, or `LEGALNOTICE` files. Redistributors must retain those
files and independently confirm that the selected components' terms are suitable
for their use. This inventory is informational and is not legal advice.

STAMP's `apps/stamp/LICENSE` refers to `ssca2/COPYRIGHT`, which is not present in
this tree. Do not redistribute that benchmark until its source and terms have
been confirmed and the notice restored.
