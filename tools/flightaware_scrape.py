#!/usr/bin/env python3
"""
Quick probe for whether FlightAware's public flight page exposes route data in
server-delivered HTML for a callsign.

Usage:
  python tools/flightaware_scrape.py UAL1941
  python tools/flightaware_scrape.py UAL1941 --save-html tmp/ual1941.html
"""

from __future__ import annotations

import argparse
import dataclasses
import html
import json
import pathlib
import re
import sys
import urllib.error
import urllib.request


USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36"
)


@dataclasses.dataclass
class Endpoint:
    icao: str = ""
    iata: str = ""
    name: str = ""
    city: str = ""


@dataclasses.dataclass
class RouteResult:
    callsign: str
    source: str = ""
    origin: Endpoint = dataclasses.field(default_factory=Endpoint)
    destination: Endpoint = dataclasses.field(default_factory=Endpoint)
    raw_bytes: int = 0
    confidence: str = "none"

    def complete(self) -> bool:
        return bool(
            (self.origin.icao or self.origin.iata or self.origin.name or self.origin.city)
            and
            (self.destination.icao or self.destination.iata or self.destination.name or self.destination.city)
        )


def fetch_html(callsign: str) -> str:
    url = f"https://www.flightaware.com/live/flight/{callsign}"
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept": "text/html,application/xhtml+xml",
            "Accept-Language": "en-US,en;q=0.9",
            "Cache-Control": "no-cache",
        },
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        charset = resp.headers.get_content_charset() or "utf-8"
        return resp.read().decode(charset, errors="replace")


def compact(text: str) -> str:
    return re.sub(r"\s+", " ", html.unescape(text)).strip()


def find_json_ld(html_text: str) -> list[dict]:
    docs: list[dict] = []
    for match in re.finditer(
        r'<script[^>]+type="application/ld\+json"[^>]*>\s*(.*?)\s*</script>',
        html_text,
        re.IGNORECASE | re.DOTALL,
    ):
        body = match.group(1).strip()
        try:
            docs.append(json.loads(body))
        except json.JSONDecodeError:
            pass
    return docs


def find_routeish_pairs(html_text: str) -> list[str]:
    patterns = [
        r'"origin"\s*:\s*"([^"]+)"',
        r'"destination"\s*:\s*"([^"]+)"',
        r'"originName"\s*:\s*"([^"]+)"',
        r'"destinationName"\s*:\s*"([^"]+)"',
        r'"route"\s*:\s*"([^"]+)"',
        r'"flightPlan"\s*:\s*"([^"]+)"',
        r'"displayName"\s*:\s*"([^"]+)"',
    ]
    hits: list[str] = []
    for pattern in patterns:
        for match in re.finditer(pattern, html_text, re.IGNORECASE):
            value = compact(match.group(1))
            if value and value not in hits:
                hits.append(value)
    return hits


def find_meta_content(html_text: str, name: str) -> str | None:
    match = re.search(
        rf'<meta\s+name="{re.escape(name)}"\s+content="([^"]+)"',
        html_text,
        re.IGNORECASE,
    )
    if not match:
        return None
    return compact(match.group(1))


def find_friendly_locations(html_text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    patterns = {
        "origin_iata": r'"origin"\s*:\s*\{.*?"iata"\s*:\s*"([^"]+)"',
        "origin_friendly": r'"origin"\s*:\s*\{.*?"friendlyLocation"\s*:\s*"([^"]+)"',
        "origin_name": r'"origin"\s*:\s*\{.*?"friendlyName"\s*:\s*"([^"]+)"',
        "destination_iata": r'"destination"\s*:\s*\{.*?"iata"\s*:\s*"([^"]+)"',
        "destination_friendly": r'"destination"\s*:\s*\{.*?"friendlyLocation"\s*:\s*"([^"]+)"',
        "destination_name": r'"destination"\s*:\s*\{.*?"friendlyName"\s*:\s*"([^"]+)"',
    }
    for key, pattern in patterns.items():
        match = re.search(pattern, html_text, re.IGNORECASE | re.DOTALL)
        if match:
            out[key] = compact(match.group(1))
    return out


def extract_endpoint(html_text: str, side: str) -> Endpoint:
    assert side in ("origin", "destination")
    ep = Endpoint()
    patterns = {
        "icao": rf'"{side}"\s*:\s*\{{.*?"icao"\s*:\s*"([^"]+)"',
        "iata": rf'"{side}"\s*:\s*\{{.*?"iata"\s*:\s*"([^"]+)"',
        "name": rf'"{side}"\s*:\s*\{{.*?"friendlyName"\s*:\s*"([^"]+)"',
        "city": rf'"{side}"\s*:\s*\{{.*?"friendlyLocation"\s*:\s*"([^"]+)"',
    }
    for field, pattern in patterns.items():
        match = re.search(pattern, html_text, re.IGNORECASE | re.DOTALL)
        if match:
            setattr(ep, field, compact(match.group(1)))
    return ep


def extract_route(html_text: str, callsign: str) -> RouteResult:
    result = RouteResult(callsign=callsign.strip().upper(), raw_bytes=len(html_text))

    meta_origin = find_meta_content(html_text, "origin")
    meta_destination = find_meta_content(html_text, "destination")
    embedded_origin = extract_endpoint(html_text, "origin")
    embedded_destination = extract_endpoint(html_text, "destination")

    if embedded_origin.icao or meta_origin:
        embedded_origin.icao = embedded_origin.icao or meta_origin or ""
    if embedded_destination.icao or meta_destination:
        embedded_destination.icao = embedded_destination.icao or meta_destination or ""

    if embedded_origin.iata or embedded_origin.name or embedded_origin.city:
        result.origin = embedded_origin
    elif meta_origin:
        result.origin.icao = meta_origin

    if embedded_destination.iata or embedded_destination.name or embedded_destination.city:
        result.destination = embedded_destination
    elif meta_destination:
        result.destination.icao = meta_destination

    if result.complete():
        result.source = "embedded-flight-state"
        result.confidence = "high"
    elif meta_origin and meta_destination:
        result.source = "meta-tags"
        result.confidence = "medium"

    return result


def route_to_jsonable(result: RouteResult) -> dict:
    return {
        "callsign": result.callsign,
        "source": result.source,
        "confidence": result.confidence,
        "rawBytes": result.raw_bytes,
        "origin": dataclasses.asdict(result.origin),
        "destination": dataclasses.asdict(result.destination),
    }


def print_route(result: RouteResult) -> None:
    print(f"Fetched {result.raw_bytes} bytes for {result.callsign}")
    if result.complete():
        print(f"Route source: {result.source} ({result.confidence})")
        print(
            "  "
            f"{result.origin.iata or '?'} / {result.origin.icao or '?'} "
            f"- {result.origin.name or '?'} - {result.origin.city or '?'}"
        )
        print("  ->")
        print(
            "  "
            f"{result.destination.iata or '?'} / {result.destination.icao or '?'} "
            f"- {result.destination.name or '?'} - {result.destination.city or '?'}"
        )
    else:
        print(f"Route source: {result.source or 'none'} ({result.confidence})")


def find_internal_endpoints(html_text: str) -> list[str]:
    endpoints: list[str] = []
    patterns = [
        r'https://[^"\']+',
        r'/live/flight/[^"\']+',
        r'/ajax/[^"\']+',
        r'/aeroapi/[^"\']+',
        r'/json/[^"\']+',
        r'/api/[^"\']+',
    ]
    for pattern in patterns:
        for match in re.finditer(pattern, html_text, re.IGNORECASE):
            value = match.group(0)
            if "flightaware.com" in value or value.startswith("/"):
                if value not in endpoints:
                    endpoints.append(value)
    return endpoints[:50]


def find_visible_clues(html_text: str) -> list[str]:
    clues: list[str] = []
    for pattern in [
        r"<title>(.*?)</title>",
        r"<h1[^>]*>(.*?)</h1>",
        r"<h2[^>]*>(.*?)</h2>",
        r"<h3[^>]*>(.*?)</h3>",
        r">([^<]{0,80}(?:Route|Origin|Destination|Departed|Arrived)[^<]{0,80})<",
    ]:
        for match in re.finditer(pattern, html_text, re.IGNORECASE | re.DOTALL):
            text = compact(re.sub(r"<[^>]+>", " ", match.group(1)))
            if text and text not in clues:
                clues.append(text)
    return clues[:30]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("callsign")
    parser.add_argument("--save-html", dest="save_html")
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()

    try:
        html_text = fetch_html(args.callsign.strip().upper())
    except urllib.error.HTTPError as exc:
        print(f"HTTP error: {exc.code}", file=sys.stderr)
        return 2
    except Exception as exc:
        print(f"Fetch failed: {exc}", file=sys.stderr)
        return 2

    if args.save_html:
        path = pathlib.Path(args.save_html)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(html_text, encoding="utf-8")
        print(f"Saved HTML to {path}")

    callsign = args.callsign.strip().upper()
    result = extract_route(html_text, callsign)
    if args.as_json:
        print(json.dumps(route_to_jsonable(result), indent=2))
    else:
        print_route(result)

    if args.debug:
        locations = find_friendly_locations(html_text)
        if locations:
            print("Friendly-location hits:")
            for key in sorted(locations):
                print(f"  {key}: {locations[key]}")

        json_ld = find_json_ld(html_text)
        print(f"JSON-LD blocks: {len(json_ld)}")
        if json_ld:
            for idx, block in enumerate(json_ld[:3], start=1):
                print(f"  JSON-LD #{idx}: top-level keys = {sorted(block.keys())[:12]}")

        routeish = find_routeish_pairs(html_text)
        print(f"Route-like JSON/string hits: {len(routeish)}")
        for item in routeish[:20]:
            print(f"  {item}")

        clues = find_visible_clues(html_text)
        print(f"Visible route clues: {len(clues)}")
        for item in clues[:20]:
            print(f"  {item}")

        endpoints = find_internal_endpoints(html_text)
        print(f"Internal/API-like endpoints found: {len(endpoints)}")
        for item in endpoints[:20]:
            print(f"  {item}")

    return 0 if result.complete() else 1


if __name__ == "__main__":
    raise SystemExit(main())
