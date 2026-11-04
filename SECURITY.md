# Security policy

OVT reads video files from anywhere, so it treats every input as hostile. **A crash, hang, out-of-bounds read
or write, or other memory error on a crafted or damaged file is a security bug**, even when the file is not a
valid stream (chapter 0 §0.7, rule 1).

## Reporting a vulnerability

Report it privately through GitHub: open the repository's **Security** tab and choose **Report a
vulnerability** ([direct link](https://github.com/openvideotools/ovt/security/advisories/new)). Please do not
open a public issue, pull request or discussion for it.

Include, if you can:

- the OVT version (`ovt --version`) and platform;
- the command or the steps in OVT Studio;
- the input file, or a description of how to make it (attach it to the advisory, which stays private);
- the crash output or sanitizer report.

## What happens next

- We acknowledge the report within 7 days.
- We agree on a fix and a disclosure date with you. The default is **90 days** after the report, or earlier
  when a fixed release is out.
- The fix ships in a release of every supported version, and the advisory is published with a CVE when one
  applies. You are credited unless you ask us not to.

If the bug is in a third-party library that OVT uses (FFmpeg, libaom, dav1d, libde265, libvmaf, libplacebo
and others), we report it upstream and coordinate with that project's own process.

## Supported versions

Before 1.0 only the latest release is supported. From 1.0 on, the latest minor release of the current major
version receives security fixes.
