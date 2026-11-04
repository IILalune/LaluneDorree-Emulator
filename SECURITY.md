# Security policy

## Supported versions

Security fixes target the latest release, currently **1.02**, and the `main` branch.
Historical release branches are archives; fixes are not routinely backported.
Please update before reporting an issue that may already have been fixed.

This is a personal, volunteer-maintained project. Response times and fixes cannot be guaranteed.

## Reporting a vulnerability

Please report suspected vulnerabilities privately. Do not put exploit files, detailed attack
instructions, private information, or commercial ROMs in public issues or pull requests.

On the repository's **Security** tab, use **Report a vulnerability** if that option is available.
If it is unavailable, open an issue titled **Private security contact requested**, without
technical details or attachments, so the maintainer can arrange a private reporting channel.

In the private report, include:

- The app version, macOS version, and Mac architecture.
- What can go wrong and the steps needed to reproduce it.
- Any relevant logs, with personal paths and information removed.
- A minimal, legally redistributable example when possible.

Test only on systems and data you own or have permission to use. Please coordinate public
disclosure with the maintainer so users have time to receive a fix. There is no paid bug bounty.

## Scope and practical limits

Reports about malicious ROMs or state files, memory corruption, unintended file access, data
loss, or sandbox escapes are welcome. Ordinary game compatibility problems belong in issues.

The downloadable app uses App Sandbox and the hardened runtime. These reduce risk but do not
make the app immune to bugs. Local development builds are not necessarily sandboxed. Keep
backups of your saves and only open files from sources you trust.
