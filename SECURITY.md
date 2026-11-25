# Security policy

## Supported versions

Security fixes go into the latest release. Please check that a problem still happens with the latest version from
[Releases](https://github.com/ycgrid/ycgrid/releases) before reporting it.

## Reporting a vulnerability

Please report security problems privately, not in a public issue:

- on GitHub: the repository's **Security** tab → **Report a vulnerability**, or
- by email to ycgrid.app@gmail.com.

Include the YCGrid version, the Windows version, the steps or a file that shows the problem, and what an attacker
gains. We aim to answer within a week, agree on a fix and a date with you, and credit you in the release notes unless
you prefer otherwise.

## What counts

YCGrid runs Python code, opens files from anywhere and talks to other processes, so these are in scope:

- Python code in a workbook's folder that runs **before** the user clicks Enable Content, or despite the Trust Center's
  settings; a file from the Internet that leaves Protected View by itself.
- Opening a crafted `.ycg`, `.xlsx`, `.csv` or `.yctape` file that crashes YCGrid in a way that could run code, or that
  reads or writes files the user didn't choose.
- An encrypted workbook (`.ycg` with a password) whose contents can be read without the password.
- Another user or a website reaching YCGrid's control pipe, the MCP server or the debugger port.
- Credentials or personal data written to logs, recordings or files where the user wouldn't expect them.

Not vulnerabilities: Python code doing what it was written to do once the user trusted it (a trusted workbook's code
has the user's rights, as an Excel macro does), and protection that Excel also documents as not being security
(sheet and workbook structure protection without encryption).
