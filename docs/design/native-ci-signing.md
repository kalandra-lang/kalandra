# GitHub Actions signing and notarization

The hosted `Native release` workflow builds the packages. A separate macOS job
imports the Developer ID identity into a temporary Keychain, signs every bundled
Mach-O with a trusted timestamp and hardened runtime, repeats runtime acceptance,
creates signed DMGs, submits them to Apple, staples the accepted tickets and runs
Gatekeeper assessment. It uploads DMGs, checksums and submission receipts as
`native-notarized-assets`. Local signing or a local notarytool profile is unnecessary.

## Configure credentials once

In Keychain Access, select the existing **Developer ID Application** identity under
**My Certificates**, including its associated private key. Export it as a
password-protected `.p12` outside the repository. Exporting the certificate alone
as `.cer` is insufficient. Keep the export password separate from the file.
Do not create another certificate when the existing identity is valid.

For this publisher the Team ID is `RWVU9G38P5`; the verified certificate SHA-1 is
`5F0A0A53D6C77D0FEF20B326AC86CA5D63BC4A7B`. Verify a replacement identity before
updating that fingerprint.

With GitHub CLI authenticated, run these commands locally. Replace the example
file path. The pipeline sends the base64 export directly to GitHub Secrets;
it does not print the private key. The remaining commands prompt for their values.
Do not supply passwords as literal shell arguments or paste them into chat.

```sh
base64 -i "/absolute/path/Developer-ID-Application.p12" | tr -d '\n' | gh secret set KALANDRA_SIGNING_P12_BASE64 --repo kalandra-lang/kalandra
gh secret set KALANDRA_SIGNING_P12_PASSWORD --repo kalandra-lang/kalandra
gh secret set KALANDRA_APPLE_ID --repo kalandra-lang/kalandra
gh secret set KALANDRA_APPLE_APP_PASSWORD --repo kalandra-lang/kalandra
gh variable set KALANDRA_APPLE_TEAM_ID --body RWVU9G38P5 --repo kalandra-lang/kalandra
gh variable set KALANDRA_SIGNING_IDENTITY --body 5F0A0A53D6C77D0FEF20B326AC86CA5D63BC4A7B --repo kalandra-lang/kalandra
```

`KALANDRA_APPLE_APP_PASSWORD` is an Apple **app-specific password** for the
Developer Apple ID, not the account's login password. Create/manage it in the
[Apple Account security settings](https://account.apple.com/). Apple's
[notarization documentation](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)
describes this authentication separately from the Developer ID signing identity.

Keep or remove the local encrypted export according to your certificate-backup
policy; never commit it. The runner deletes its imported Keychain and export in
both the script's cleanup and an `always()` workflow step. Credential command
errors suppress command arguments and output. No secrets are attached as artifacts.

## Rehearse without publishing

After all four secrets and both identity variables are configured, enable branch
notarization:

```sh
gh variable set KALANDRA_NOTARIZE --body true --repo kalandra-lang/kalandra
```

Then rerun the latest `Native release` run on `native`, or push a qualification
change. Native branch runs upload workflow artifacts only. Manual dispatch also
requires this variable, and is available once the workflow exists on the default
branch. Version tags always require notarization; missing credentials fail the
job and block publication. Only a successful tagged run can publish a release.

Unsigned `.tar.gz` artifacts retain deterministic repack qualification. Trusted
signing timestamps and notarization tickets make the signed DMGs different
deliveries; their receipts bind them to the exact unsigned input hashes. This
workflow covers standalone mixed CPU/Metal, CPU and editor distributions. The
macOS 26 hosted qualification, physical GPU execution and downloaded quarantine
checks are tracked in the [N5 record](native-distribution-qualification.md).

Hosted macOS 26 run [39973422436](https://github.com/kalandra-lang/kalandra/actions/runs/39973422436)
successfully signed and notarized all three distributions. Stapling, ticket
validation and hosted Gatekeeper assessment passed. No signing or notarization
occurred locally. Local quarantine and physical GPU checks passed using the exact
downloaded signed artifacts, as recorded in the N5 evidence.
