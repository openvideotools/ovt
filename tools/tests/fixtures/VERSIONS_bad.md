# Negative fixture of the licence gate (WI-0.032)

A pins file with a forbidden dependency.

CI runs `tools/license_gate.py --versions tools/tests/fixtures/VERSIONS_bad.md` and requires exit 1 naming the
`fake-agpl-service` row. This is not a real dependency and never feeds a build.

| Name | Version | Licence | URL | Patches | SHA-256 | Build | Notes |
|---|---|---|---|---|---|---|---|
| fake-agpl-service | 1.0.0 | AGPL-3.0-only | <https://example.org/fake-agpl-service> | – | – | none | fixture only |
