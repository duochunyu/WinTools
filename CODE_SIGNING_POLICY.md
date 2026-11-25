# Code signing policy

This page describes how WinTools releases are signed, who may approve a signature, and how to check
a download. SignPath Foundation asks applicants to publish such a policy, so it is kept in the
repository rather than in a wiki.

## Current status

The signing pipeline is in place and the certificate is not: as of **v0.3.0 (2026-11-25)** releases
are published **unsigned**, and the release notes say so plainly instead of implying otherwise. The
application for a SignPath Foundation certificate is pending. Everything below describes the state
the project moves to once that certificate exists; the verification steps it promises are already
enforced by the release workflow, which aborts the build rather than publish a binary that fails
them.

## What is signed

A signed `WinTools.exe` carries an Authenticode signature from a code signing certificate issued to
**SignPath Foundation**, accompanied by an RFC 3161 timestamp. The certificate's private key lives in
SignPath's HSM; this project never holds it and never sees it.

The publisher name shown by Windows is therefore `SignPath Foundation`, not an individual. That is
the intended arrangement for free open-source signing and is not a sign of an impersonated build.

## Who can submit and who can approve

| Role | Who | What they can do |
|---|---|---|
| Author / committer | Repository maintainer | Push code, trigger builds, open a signing request |
| Reviewer | Repository maintainer | Review the change that is about to be released |
| Approver | Repository maintainer | Approve a signing request in the SignPath web UI |

**Limitation, stated plainly:** this is a single-maintainer project, so the same person currently
holds all three roles. The separation that matters in practice is between *the build* and *the
signature*: the CI job can only *request* a signature, and no signature is produced until a human
approves it in a separate account protected by multi-factor authentication. A leaked CI token or a
compromised workflow therefore cannot produce a signed release on its own.

If the project gains further maintainers, approval moves to a second person and this table is
updated before that change is announced.

## Build provenance

Only builds that satisfy all of the following can be signed:

- Built on a GitHub-hosted runner. Self-hosted runners are not used.
- Triggered by pushing a tag matching `v*`.
- The tag equals `project(VERSION)` in `CMakeLists.txt`. A mismatch fails the build, so the version
  a user sees in the file properties is the version the release claims to be.
- SignPath independently verifies the repository, commit, ref and runner type before it accepts the
  request.

After signing, the workflow re-checks the returned binary before anything is published: the
signature must report `Valid`, must carry a timestamp, must name the expected publisher, and the
embedded file version must still match the tag. Any failure aborts the release instead of shipping
an unsigned or stale binary under a signed release.

The SHA-256 published in the release notes is computed **after** signing, so it describes the exact
bytes that were signed and shipped.

## Verifying a download

```powershell
Get-FileHash .\WinTools.exe -Algorithm SHA256     # compare with the release notes
Get-AuthenticodeSignature .\WinTools.exe | Format-List Status, StatusMessage, SignerCertificate
```

For a signed release `Status` reads `Valid` and the signer is `SignPath Foundation`. **While the
certificate is pending, releases are unsigned and `Status` reads `NotSigned`** - that is expected
today, not a sign of tampering, and the release notes state it on every download. In that state the
SHA-256 is the only check available, so compare it.

A signed binary still has to earn SmartScreen reputation, and the first release under a new signing
identity may warn once. That warning names the publisher; it does not say "unknown publisher".

Until signing is active, read that sentence the other way round: a download that reports "unknown
publisher" may well be ours, because none of them are signed yet. Verify the SHA-256.

## Privacy

WinTools collects nothing. It contains no telemetry, analytics or crash reporting. The only network
access the application can make is the update check, which is **off by default** and can be turned
off again at any time in the settings panel. See [PRIVACY.md](PRIVACY.md) for exactly what is stored
on disk and what the update request sends.
