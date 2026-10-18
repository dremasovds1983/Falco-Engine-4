# Clean source snapshots

Requires PowerShell 7. The reviewed JSON manifest is a separate input, kept outside
the copied tree along with the validation report. Each file has an exact relative
path, byte count, SHA-256, disposition (OWN, THIRD_PARTY, REVIEW), and reason.
All listed files are required. New files require an explicit inventory update.

```powershell
./BuildCleanTree.ps1 -SourceRoot . -Destination ./out/source-snapshot -ManifestPath ./reviewed-manifest.json -ReportPath ./out/source-report.json -Purpose Private
./ValidateCleanTree.ps1 -Root ./out/source-snapshot -ManifestPath ./reviewed-manifest.json -ReportPath ./out/source-recheck.json -Purpose Private
```

Destination must be new. Files outside the inventory are not copied. Reparse
points, metadata, excluded SDK roots, binary build outputs and secret filenames
are rejected. Validation also reports unknown files, missing files, changed hashes,
credential-shaped content and local machine paths. Findings never echo credentials.
The scan is a defined set of checks, not a guarantee that arbitrary secrets are absent.

Private mode permits explicitly labeled REVIEW inputs. Public mode rejects them
and unresolved publicationBlockers. A technical public composition pass does not
grant rights or authorize publishing. Actual publication is a separate action.

Validate the pristine source tree before bootstrapping: generated out/deps-cache
content is deliberately forbidden in the source package. Dependency bootstrap and
rebuild are separate gates. The private snapshot keeps legacy conditional source
branches, but excludes their SDKs and the legacy CSG comparison corpus; it targets
the modern Engine/Player/Editor/ShaderSmoke build, not the Carve comparison test.
