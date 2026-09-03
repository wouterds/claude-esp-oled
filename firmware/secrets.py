"""Turns firmware/.env into a header the build can include.

Generated into the build directory rather than into src, so nothing secret ever
sits beside the code where it could be committed by accident. A checkout with no
.env still builds - it just has no networks to join.
"""

import os

Import("env")  # noqa: F821  - injected by PlatformIO

# __file__ is not defined in the context PlatformIO runs this in.
HERE = env.subst("$PROJECT_DIR")  # noqa: F821


def read_env(path):
    values = {}
    if not os.path.exists(path):
        return values
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            # Only the line ending comes off the value. .env.example promises a
            # password may contain whatever a delimiter would have been, and a
            # trailing space is one of those - stripped here it joins nothing and
            # the file it came from still looks right.
            values[key.strip()] = value.rstrip("\r\n")
    return values


def quote(value):
    # A C string literal, so a backslash or a quote in a password cannot end it
    # early and turn the rest of it into code.
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return '"{}"'.format(escaped)


values = read_env(os.path.join(HERE, ".env"))

networks = []
index = 1
while True:
    ssid = values.get("WIFI_{}_SSID".format(index))
    password = values.get("WIFI_{}_PASS".format(index))
    if not ssid:
        break
    networks.append((ssid, password or ""))
    index += 1

out_dir = os.path.join(env.subst("$BUILD_DIR"), "generated")  # noqa: F821
os.makedirs(out_dir, exist_ok=True)
path = os.path.join(out_dir, "secrets.h")
body = (
    "// Generated from firmware/.env by secrets.py. Do not edit.\n"
    "#pragma once\n\n"
    "static const WifiNetwork WIFI_NETWORKS[] = {\n"
)
for ssid, password in networks:
    body += "    {{{}, {}}},\n".format(quote(ssid), quote(password))
if not networks:
    body += "    {nullptr, nullptr},\n"
body += "};\n"

# Only when it has actually changed, the same as version.py: rewritten every
# build, the header's mtime moves every build and wifi.cpp is recompiled for
# nothing every time anything at all is built.
if not os.path.exists(path) or open(path, encoding="utf-8").read() != body:
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(body)

env.Append(CPPPATH=[out_dir])  # noqa: F821
print("secrets.py: {} network(s) from .env".format(len(networks)))
