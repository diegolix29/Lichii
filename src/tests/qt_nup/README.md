# Synthetic NUP fixture

`shifted_base_update.nup` was made with `nup pack` from
[a55uka/nup at a4840f0](https://github.com/a55uka/nup/tree/a4840f06c94ddeb414eea5b33c9704ffe995f11c).
It contains only synthetic ExeFS headers in `base/` and `updates/327680/`.
That version of the packer stores Build IDs under type `0x0006` instead of
`0x0007`. The base ID includes nine `FF` bytes so an unintended UTF-8
round trip expands the ID from 32 to 50 bytes.

`spec_base_update.nup` contains the same files packed by
[a55uka/nup at a34925a](https://github.com/a55uka/nup/tree/a34925ad09746cac0c65805ef2f6f19898eff78c),
before the numbering regression, with `--publisher 'Publisher with exactly 32 bytes!'`.
The spec-numbered control verifies that the publisher length alone does not block an edit.
