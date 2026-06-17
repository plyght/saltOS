# saltOS Trust and Contribution Model

saltOS uses an **explicit, boring** trust system for repository contribution.
The goal is not a clever automated reputation engine; it is a small set of
understandable rules: signed releases, reviewed recipes, vouched contributors,
logged decisions, and no drive-by package ownership takeover.

The model borrows ideas from trust-list systems like **Vouch** (people vouch for
people) and from policy/integrity engines like **Tripwire** (declared baselines,
detect unexpected change) — without blindly copying either. Vouch inspires the
contributor trust graph; Tripwire inspires the supply-chain change detection.

See also: [repository.md](repository.md), [contributing.md](contributing.md),
[recipes.md](recipes.md).

## Contributor trust levels

Every contributor has a trust level. These map directly to the `salt_trust_level`
enum in `include/salt/trust.h`:

| Level | Enum value | Meaning |
| --- | --- | --- |
| `unknown` | `SALT_TRUST_UNKNOWN` = 0 | Default. May open issues and proposals, but cannot land recipe or code changes without review. |
| `vouched` | `SALT_TRUST_VOUCHED` = 1 | Vouched for by an existing trusted contributor. Carries more weight in review; still reviewed. |
| `maintainer` | `SALT_TRUST_MAINTAINER` = 2 | Trusted to review and approve changes, including package admission. |
| `denounced` | `SALT_TRUST_DENOUNCED` = -1 | Explicitly distrusted. Contributions are blocked. |

The trust database is queried and mutated through:

- `salt_trust_lookup(trustdb_path, contributor)` — resolve a contributor's
  current level.
- `salt_trust_set(trustdb_path, contributor, lvl, by, reason)` — set a
  contributor's level, recording **who** made the change (`by`) and **why**
  (`reason`). Decisions are logged.
- `salt_trust_vouch(trustdb_path, voucher, contributor, reason)` — record that
  one contributor vouches for another, raising them toward `vouched`.

`salt_trust_level_name` and `salt_trust_level_parse` convert between the enum and
its string form for display and configuration.

Unknown contributors are not shut out of the project — they may open issues or
package proposals depending on project policy — but **package recipe changes
require review from trusted maintainers.** There is no path by which an unknown
contributor silently takes ownership of an existing package.

## Package admission rules

A package does not enter the official repository unless **all** of the following
hold (DISTRO §11.2):

- the source URL is pinned;
- the source hash is pinned;
- the license is declared;
- build dependencies are declared;
- runtime dependencies are declared;
- the build succeeds in a clean environment;
- the package contents are inspectable;
- a maintainer or trusted reviewer approves it.

`salt_recipe_admission_check(recipe_path, out)` evaluates the mechanically
checkable subset of these rules against a recipe and reports findings. The human
approval and the clean-environment build are enforced by the review workflow and
CI respectively; admission is the combination of the automated check passing and
a trusted maintainer approving.

## Supply-chain risk rules

The repository tooling watches for the patterns that historically precede
supply-chain compromise (DISTRO §11.3). `salt_supplychain_scan` flags:

- maintainer changes;
- abandoned packages;
- source URL changes;
- new network access during build;
- new install scripts;
- large unexpected file changes;
- generated binary blobs;
- crypto wallet addresses;
- obfuscated scripts;
- sudden package ownership changes;
- suspicious contributor behavior.

The scan takes a `salt_scan_input`:

```c
typedef struct {
  char *recipe_path;            // the recipe being scanned
  const char *prev_recipe_text; // the previous version, for change detection
  salt_trust_level author_level;// trust level of whoever authored the change
} salt_scan_input;
```

Providing `prev_recipe_text` is what lets the scan behave like a Tripwire-style
baseline: it can detect a *changed* source URL, a *newly added* install script,
or a *sudden* maintainer/ownership change, not just absolute properties. The
`author_level` lets the scan weigh a change differently depending on whether it
came from an `unknown` contributor or a `maintainer`.

### Findings and severities

Results come back as `salt_findings` — a list of `salt_finding`, each with a
severity, a stable `code`, and a human-readable `message`:

```c
typedef enum {
  SALT_RISK_INFO,
  SALT_RISK_WARN,
  SALT_RISK_BLOCK,
} salt_risk_severity;
```

- `INFO` — noted for the reviewer; not a problem by itself.
- `WARN` — requires attention and likely extra review.
- `BLOCK` — stops admission.

`salt_findings_has_block(findings)` reports whether any blocking finding is
present. **If a scan produces a `BLOCK` finding, the package is not admitted**
until the underlying issue is resolved or a maintainer makes an explicit,
logged decision to override after review.

### The three checks, compared

| Function | Question it answers |
| --- | --- |
| `salt_recipe_lint` | Is this recipe well-formed and internally valid? (schema, required fields, sane values) |
| `salt_recipe_admission_check` | Does this recipe satisfy the mechanical admission rules? (pinned URL/hash, declared license and deps, inspectable, etc.) |
| `salt_supplychain_scan` | Does this *change* exhibit risky supply-chain patterns relative to the previous version and the author's trust? |

Lint is about correctness, admission is about policy compliance, and the scan is
about change-driven risk.

## No blind automation

Automated scoring can **warn**, **block**, or **require extra review** — but it
is never the **sole** basis for trust (DISTRO §11.4). A clean scan does not
auto-merge a package, and a passing admission check does not bypass human
approval. The trust model stays explicit and boring:

- signed releases;
- reviewed recipes;
- vouched contributors;
- logged decisions;
- no drive-by package ownership takeover.

Automation exists to surface risk to a human reviewer, not to replace the
reviewer.

## Security policy

The initial security baseline (DISTRO §12) is:

- signed repository metadata;
- source hashes required;
- build sandboxing required for official builds;
- no arbitrary install scripts by default;
- a package contents manifest required;
- transaction rollback required;
- the official repository curated by trusted maintainers.

Future goals include reproducible builds for critical packages, multiple-builder
verification, a binary transparency log, package provenance records, SBOM
generation, mandatory review for high-risk packages, and an automated policy
engine for repository changes.

## The `salt trust` CLI surface

Trust operations are exposed under `salt trust <subcommand>`. Typical
invocations:

```sh
# Scan a recipe change for supply-chain risk (uses prev version + author level)
salt trust scan recipes/helium

# Run the mechanical admission check against a recipe
salt trust admit recipes/zlib

# Look up a contributor's current trust level
salt trust lookup alice@example.org

# Set a contributor's trust level, with attribution and a reason (logged)
salt trust set alice@example.org maintainer --by bob@example.org --reason "long-time reviewer"

# Vouch for a contributor
salt trust vouch alice@example.org --reason "verified identity, good track record"
```

Every mutating operation records who made the change and why, so the trust state
of the project is always inspectable and auditable.
