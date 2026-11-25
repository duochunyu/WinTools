# Code signing policy

WinTools releases are Authenticode-signed. This page describes who signs them, what is guaranteed,
and how to check a download. SignPath Foundation asks applicants to publish such a policy, so it is
kept in the repository rather than in a wiki.

## What is signed

Every published `WinTools.exe` is signed with a code signing certificate issued to **SignPath
Foundation** and timestamped through an RFC 3161 timestamp authority. The certificate's private key
lives in SignPath's HSM; this project never holds it and never sees it.

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

`Status` should be `Valid` and the signer should be `SignPath Foundation`.

A signed binary still has to earn SmartScreen reputation, and a first release under a new signing
identity may warn once. The warning names the publisher in that case; it does not say "unknown
publisher". A build that says "unknown publisher" is not one of ours.

## Privacy

WinTools collects nothing. It contains no telemetry, analytics or crash reporting. The only network
access the application can make is the update check, which is **off by default** and can be turned
off again at any time in the settings panel.
