# Safety artifacts

GPTPS is open source under the MIT license, and it stays that way. Separately, a
**GPTPS Safety Artifacts Package** is being prepared. It is planned to hold the
GPTPS-level evidence a product team typically needs when GPTPS is part of a product
going through functional-safety certification. The package is its own product, under its
own commercial license. This page says what it will be, how it is licensed, and what it
does not claim.

## Status

The package is in preparation. **No GPTPS release is certified today**, and nothing in
this repository is a certification claim. Each package, when there is one, names the
exact GPTPS release it covers.

## Two products, two licenses

| | License | Where |
|---|---|---|
| GPTPS source code, documentation, tests and CI | MIT ([LICENSE](../LICENSE)) | this repository |
| GPTPS Safety Artifacts Package | GPTPS Safety Artifacts License (commercial) | delivered to licensees, never in this repository |

The GPTPS Safety Artifacts License never restricts GPTPS itself. Anyone may use,
modify and ship GPTPS under the MIT license, commercially too, with or without the
package, and the source of a release the package covers stays MIT. You never need the
package to use GPTPS.

## What a package contains

Each package belongs to one GPTPS release and is planned to contain:

- the safety manual and safety plan;
- software safety requirements and architecture documentation;
- requirements traceability;
- the verification plan and report, test specifications and results, and coverage
  reports;
- static-analysis results and coding-standard compliance evidence, and formal
  verification reports where they apply;
- known anomalies and deviations;
- configuration and baseline information, and change-impact analysis between
  releases;
- third-party assessment reports and certificates, where they exist.

## How it is licensed

This summarizes the model. The GPTPS Safety Artifacts License agreement itself governs.

- **Per release.** Each package covers one specific GPTPS release. An artifact release
  is one issue of a package: its first issue, or a later correction or update. A
  license covers the artifact releases delivered under it, within the scope it states.
- **Scope.** A license is scoped to one product, one product family, or one
  organization.
- **Subscription and perpetual rights.** An annual subscription delivers, during the
  paid term, new artifact releases (corrections and updates to the current package,
  and packages for later GPTPS releases) and support on the artifacts. The license to
  each artifact release received during a paid term is perpetual within the license's
  scope. Ending the subscription stops delivery of new artifact releases and support.
  It does not withdraw the right to use artifact releases received during a paid term:
  the licensee may keep using them within that scope, including for a product already
  certified with them.
- **Use with assessors.** A licensee may share the artifacts, in whole or in part, with
  its employees, contractors, certification bodies, assessors, auditors and regulatory
  authorities, as needed to assess or certify the products within the license's scope.
- **License, not ownership.** A licensee receives the right to use the artifacts, not
  ownership of them.

## What the package is not

The artifacts are evidence and documentation for a licensee's own safety lifecycle and
certification. They are not a certificate for the licensee's product. The licensee
remains responsible for system-level safety, integration, configuration, application
software and hardware, and for obtaining any certification its product requires.
Evidence about a component, even a certified one, is one input to a product's
certification, never a substitute for it.

## Contact

To ask about the package, contact the maintainer, [@Fikoko](https://github.com/Fikoko),
through GitHub.
