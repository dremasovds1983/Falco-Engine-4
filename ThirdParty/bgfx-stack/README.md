# Graphics stack source notices

FalcoEngine uses pinned bx, bimg and bgfx. Their root BSD-2-Clause notices and
embedded source notices are preserved under the corresponding directories.
Each PROVENANCE.json identifies the exact upstream commit, archive URL/hash,
original path and, for extracted headers, the original byte interval/hash.

This is a conservative source-notice collection. It includes notices for optional
tools, examples and other platforms present in those archives; inclusion here
does not assert that every listed component is used in an application binary.
Upstream alternatives are preserved. These notices do not change third-party
code to the Falco project's Apache-2.0 license.

Catch2's original header references Boost Software License 1.0; its complete text
is provided in BSL-1.0.txt, matching the project's independently pinned Boost
license. Catch2 belongs to upstream tests, not the reviewed production projects.
D3D4Linux's header references WTFPL-2.0; that Linux implementation is not part of
the reviewed Windows projects. A future Linux/source candidate must review its
complete notice requirements independently.

Per-file attribution and exact publication-candidate coverage remain subject to
the publication audit. This collection is not a final release approval.
