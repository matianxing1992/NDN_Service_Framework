#!/usr/bin/env python3

import argparse
import base64
import os
import pwd
import sys


def add_sudo_user_site_packages():
    sudo_user = os.environ.get("SUDO_USER")
    if not sudo_user:
        return
    try:
        user_home = pwd.getpwnam(sudo_user).pw_dir
    except KeyError:
        return
    user_site = os.path.join(
        user_home, ".local", "lib",
        "python{}.{}".format(sys.version_info.major, sys.version_info.minor),
        "site-packages")
    if os.path.isdir(user_site) and user_site not in sys.path:
        sys.path.append(user_site)


add_sudo_user_site_packages()
from ndn.app import NDNApp
from ndn.encoding import Name, parse_data


def load_cert_packet(path):
    with open(path, "rb") as f:
        text = b"".join(line.strip() for line in f if not line.startswith(b"-----"))
    return base64.b64decode(text)


def main():
    parser = argparse.ArgumentParser(description="Serve a certificate Data packet by exact name")
    parser.add_argument("--cert-file", required=True, help="base64 certificate file from ndnsec")
    parser.add_argument("--expected-name", required=True, help="exact certificate Data name")
    args = parser.parse_args()

    packet = load_cert_packet(args.cert_file)
    cert_name, _, _, _ = parse_data(packet)
    cert_name_uri = Name.to_str(cert_name)
    if cert_name_uri != args.expected_name:
        print("CERT_SERVER_NAME_MISMATCH expected={} actual={}".format(
            args.expected_name, cert_name_uri), flush=True)
        return 2
    try:
        key_index = next(i for i, comp in enumerate(cert_name) if bytes(comp) == b"\x08\x03KEY")
        route_name = cert_name[:key_index + 2]
    except (StopIteration, IndexError):
        route_name = cert_name
    route_name_uri = Name.to_str(route_name)

    app = NDNApp()
    served = {"count": 0}

    @app.route(route_name)
    def on_interest(name, _param, _app_param):
        interest_name = Name.to_str(name)
        if len(name) > len(cert_name) or cert_name[:len(name)] != name:
            print("CERT_SERVER_IGNORE interest={} cert={}".format(
                interest_name, cert_name_uri), flush=True)
            return
        served["count"] += 1
        print("CERT_SERVER_INTEREST interest={} cert={} count={}".format(
            interest_name, cert_name_uri, served["count"]), flush=True)
        app.put_raw_packet(packet)
        print("CERT_SERVER_DATA interest={} cert={} bytes={}".format(
            interest_name, cert_name_uri, len(packet)), flush=True)

    print("CERT_SERVER_READY route={} name={} file={}".format(
        route_name_uri, cert_name_uri, args.cert_file), flush=True)
    try:
        app.run_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
