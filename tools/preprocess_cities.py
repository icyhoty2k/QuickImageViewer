"""
Preprocesses GeoNames data files into compact zlib-compressed binaries
for embedding as Windows RCDATA resources.

Usage (run from the project root):
    python tools/preprocess_cities.py

Input files (all in resources/geoData/):
    cities1000.txt      - city database
    admin1CodesASCII.txt - state/province names
    admin2Codes.txt      - district/county names
    countryInfo.txt      - country details

Output (in resources/geoData/):
    cities.bin, admin1.bin, admin2.bin, country.bin
"""

import zlib
import struct
import sys
import os

GEO_DIR = os.path.join('resources', 'geoData')

CONTINENTS = {
    'AF': 'Africa', 'AN': 'Antarctica', 'AS': 'Asia',
    'EU': 'Europe', 'NA': 'North America', 'OC': 'Oceania', 'SA': 'South America',
}

def compress_and_write(data_str, output_path):
    raw        = data_str.encode('utf-8')
    compressed = zlib.compress(raw, level=9)
    output     = struct.pack('<I', len(raw)) + compressed
    with open(output_path, 'wb') as f:
        f.write(output)
    print(f"  {os.path.basename(output_path)}: "
          f"{len(raw)//1024} KB raw → {len(compressed)//1024} KB compressed")

def process_cities():
    path = os.path.join(GEO_DIR, 'cities1000.txt')
    lines = []
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) < 18:
                continue
            try:
                lat = float(p[4])
                lon = float(p[5])
            except ValueError:
                continue
            name    = p[2][:60]   # asciiname
            country = p[8]        # ISO country code
            admin1  = p[10]       # state code
            admin2  = p[11]       # district code
            tz      = p[17]       # timezone
            if not name or not country:
                continue
            lines.append(f"{lat:.4f}\t{lon:.4f}\t{name}\t{country}\t{admin1}\t{admin2}\t{tz}")
    compress_and_write('\n'.join(lines), os.path.join(GEO_DIR, 'cities.bin'))
    print(f"  {len(lines):,} cities")

def process_admin1():
    path = os.path.join(GEO_DIR, 'admin1CodesASCII.txt')
    lines = []
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) < 2:
                continue
            code = p[0]   # e.g. "US.CA"
            name = p[1]   # e.g. "California" (UTF-8)
            if code and name:
                lines.append(f"{code}\t{name}")
    compress_and_write('\n'.join(lines), os.path.join(GEO_DIR, 'admin1.bin'))
    print(f"  {len(lines):,} admin1 entries")

def process_admin2():
    path = os.path.join(GEO_DIR, 'admin2Codes.txt')
    lines = []
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) < 2:
                continue
            code = p[0]   # e.g. "US.CA.037"
            name = p[1]   # e.g. "Los Angeles County" (UTF-8)
            if code and name:
                lines.append(f"{code}\t{name}")
    compress_and_write('\n'.join(lines), os.path.join(GEO_DIR, 'admin2.bin'))
    print(f"  {len(lines):,} admin2 entries")

def process_countries():
    path = os.path.join(GEO_DIR, 'countryInfo.txt')
    lines = []
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            if line.startswith('#'):
                continue
            p = line.rstrip('\n').split('\t')
            if len(p) < 13:
                continue
            iso       = p[0]
            name      = p[4]
            capital   = p[5]
            continent = CONTINENTS.get(p[8], p[8])
            curr_code = p[10]
            curr_name = p[11]
            phone     = '+' + p[12] if p[12] else ''
            if iso and name:
                currency = f"{curr_code} ({curr_name})" if curr_code and curr_name else curr_code
                lines.append(f"{iso}\t{name}\t{capital}\t{continent}\t{currency}\t{phone}")
    compress_and_write('\n'.join(lines), os.path.join(GEO_DIR, 'country.bin'))
    print(f"  {len(lines):,} countries")

def main():
    for fname in ['cities1000.txt', 'admin1CodesASCII.txt', 'admin2Codes.txt', 'countryInfo.txt']:
        p = os.path.join(GEO_DIR, fname)
        if not os.path.exists(p):
            print(f"ERROR: {p} not found.")
            sys.exit(1)

    print("Processing cities1000.txt...")
    process_cities()
    print("Processing admin1CodesASCII.txt...")
    process_admin1()
    print("Processing admin2Codes.txt...")
    process_admin2()
    print("Processing countryInfo.txt...")
    process_countries()
    print("\nDone. Place the .bin files stay in resources/geoData/ — resource.rc references them there.")

if __name__ == '__main__':
    main()
