# CI and releases

`Build APK` runs the build, tests, and public test-key signature checks on forks
as well as upstream. Download its `droiddeck-apk` artifact from the Actions run.
This test key is not the release key, so these APKs cannot replace an installed
app signed with the upstream release key.

Automatic main and PR signing, PR release cleanup, and app catalog updates run
only in `Droid-Deck/DroidDeck`. They use the upstream signing identity and GitHub
App to publish to `Droid-Deck/DroidDeck-CI`. Their jobs are intentionally skipped
on forks, including manual `PR Release Publish` and `Update app catalog` runs.
The `publish-test` label does not enable upstream publication from a fork.

`Release APKs` remains available as a manual workflow on `main`. It publishes to
the repository where it runs and requires a successful build of that exact
commit, release notes, and a configured `release` signing environment. Missing
keys and mismatched signing fingerprints remain errors. Fork releases do not
update the upstream catalog.

A fork that wants its own signed releases must deliberately configure its own
signing identity and matching public fingerprint before using that workflow.
`tools/release/make-release-key.sh` currently targets `Droid-Deck/DroidDeck`; do
not run it unchanged to configure a fork. Automatic fork publishing would also
need its own publication destination and app update source: the app currently
reads the upstream catalog. Repository guards do not establish that release
infrastructure or change the app's update source.
