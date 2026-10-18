# FreeImage acknowledgement and source information

FalcoEngine uses FreeImage 3.18.0 under the FreeImage Public License, version 1.0.
FreeImage project: https://freeimage.sourceforge.io/ . The chosen text is provided
in license-fi.txt. Bundled codecs retain their separate license notices in Source/.

For the dual-licensed LibRawLite code bundled with this version, FalcoEngine
selects CDDL-1.0. This includes DHT and AAHD, whose own file headers explicitly
permit that choice. DCB and X3F retain their separate BSD terms, reproduced in
extracted-notices/LibRawLite-internal-dcb_demosaicing.c.txt and
extracted-notices/LibRawLite-internal-libraw_x3f.cpp.txt. Other contributor headers
are preserved alongside them. The upstream LGPL alternative is retained as
received; it is not the selected license for the dual-licensed code here.

Recipients can obtain the complete LibRawLite source locally by extracting
source/FreeImage3180.zip and opening FreeImage/Source/LibRawLite/. Its CDDL text is
also available here at Source/LibRawLite/LICENSE.CDDL. That covered source is
provided under CDDL-1.0, with its original notices preserved; the project's
Apache-2.0 license does not replace its terms. No Falco modifications were found
in the archive's LibRawLite files in the reviewed build source tree.

The accompanying source/ directory contains the original pinned FreeImage ZIP,
the exact patch applied by Falco's build, build-support scripts and a SHA-256
manifest. The archive includes the bundled codecs and their original notices.
source/bundle-manifest.json records the upstream URL, archive identity and every
delivered file. No continued availability of an external download is assumed by
this local source delivery.

Modification approved 2026-07-29 (Falco project decision DEC-035): remove the
obsolete std::binary_function inheritance from NameCompare in
Source/OpenEXR/IlmImf/ImfAttribute.cpp, allowing compilation with MSVC v143.
The comparison function is unchanged. This modifies the OpenEXR code originally
copyright Industrial Light & Magic; its original copyright and BSD-style notices
are retained in the archive and under Source/OpenEXR/Copyrights/.
The Windows patch step also normalizes this file from LF to CRLF line endings;
the recorded comparison found no other source changes in that file.

Build context: the Falco source project's BuildDeps.ps1 builds wave W3 with the
locked Windows x64 Release /MT settings. From that complete project, use
BuildDeps.ps1 -Mode Build -Wave W3 after its documented prerequisite setup.
The source/build-support copy records the recipe; it is not a standalone Falco
source checkout. The full source project is needed to run all bootstrap checks.

Source files and modifications retain their applicable upstream terms; this
document does not replace them with Apache-2.0. Final distribution review still
needs to resolve all bundled-codec obligations and exact candidate composition.
