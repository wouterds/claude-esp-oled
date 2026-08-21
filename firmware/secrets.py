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
            values[key.strip()] = value.strip()
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
with open(os.path.join(out_dir, "secrets.h"), "w", encoding="utf-8") as handle:
    handle.write("// Generated from firmware/.env by secrets.py. Do not edit.\n")
    handle.write("#pragma once\n\n")
    handle.write("static const WifiNetwork WIFI_NETWORKS[] = {\n")
    for ssid, password in networks:
        handle.write("    {{{}, {}}},\n".format(quote(ssid), quote(password)))
    if not networks:
        handle.write("    {nullptr, nullptr},\n")
    handle.write("};\n")

env.Append(CPPPATH=[out_dir])  # noqa: F821
print("secrets.py: {} network(s) from .env".format(len(networks)))
