# Security policy

## Supported use

This project is a compatibility port of a deprecated scripting engine. It is
intended for trusted application scripts required by existing Qt applications.

The embedded JavaScriptCore snapshot dates from 2011 and does not receive the
security maintenance of a current JavaScript runtime. Do not expose it to
untrusted scripts, documents, plug-ins, or network-provided content, and do not
treat it as a sandbox or security boundary.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting for this repository rather
than opening a public issue with exploit details.
