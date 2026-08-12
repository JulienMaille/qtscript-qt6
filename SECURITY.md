# Security policy

## Supported use

This branch combines a deprecated QtScript API with a pinned QuickJS-NG
runtime. It is intended for trusted application scripts required by existing
Qt applications.

The QObject bridge can expose application objects and native methods directly
to JavaScript. Neither that bridge nor the runtime limits authority, CPU time,
or memory strongly enough to be a security boundary. Do not expose this module
to untrusted scripts, documents, plug-ins, or network-provided content, and do
not treat it as a sandbox. Review and update the pinned QuickJS-NG revision as
part of normal dependency maintenance.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting for this repository rather
than opening a public issue with exploit details.
