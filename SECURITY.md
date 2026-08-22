# Security Policy

This is the security policy for Sequentia Core, the node in this repository.
It replaces the Elements policy this repository inherited: vulnerabilities in
Sequentia must not be reported to Blockstream, which does not maintain this
fork.

## Reporting a vulnerability

Report privately, through either channel:

- **GitHub private vulnerability reporting** on this repository
  (Security tab, "Report a vulnerability"). The report is visible only to the
  maintainers until it is published.
- **Emissio**, the Sequentia community rewards platform, at
  https://sequentiatestnet.com/emissio/ — its private security-report form
  is visible only to you and the reviewers, and accepted reports are rewarded
  in Sequence tokens by severity tier (up to the top tier for a
  consensus-breaking or funds-loss bug with a working proof of concept).

Do not open a public issue or pull request for a vulnerability, and do not
post it in a public chat.

In your report, include as much as you can:

- a description of the vulnerability and how it could be exploited
- its potential impact (consensus split, theft of funds, denial of service,
  privacy leak)
- steps or code for reproducing it — a functional test under
  `test/functional/` is ideal
- a proposed patch, if you have one

and a way for us to reach you with follow-up questions.

## Scope

Everything in this repository: consensus and validation, Bitcoin anchoring,
proof of stake and the committee, the open fee market, supervised assets, the
P2P and RPC layers, the wallet and the GUI, and the bundled price server. Bugs
inherited from Bitcoin Core or Elements are in scope here too, because the
fix has to land in this tree.

The other Sequentia ecosystem repositories (wallets, SeqDEX, SeqLN, OpenAMP,
bridges) have their own code; report a bug there through the same two
channels and name the repository.

## Considerations

Everything here is testnet software with no real value at stake, so
responsible disclosure is about keeping the public testnet and its users
working, not about funds. Even so:

- Do not include private keys or personal data in stack traces, logs or
  exploit scripts.
- Give us a reasonable time to investigate and fix before disclosing
  elsewhere, and tell us in advance if you intend to.
- Test against a local regtest or custom chain rather than against the public
  testnet where you can.

## Verifying releases

Release artifacts on https://sequentiatestnet.com/download/ ship with a
`SHA256SUMS` file and a detached signature; the signing key's fingerprint is
published on the download page. A binary whose checksum is not in a signed
`SHA256SUMS` is not a release.
