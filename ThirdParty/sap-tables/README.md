# SAP protocol constant tables

These files are interoperability constants for Apple's FairPlay SAP
(protocol version 200) setup/signing algorithm: fixed substitution and
routing tables with no executable code. They are required byte-for-byte to
interoperate with Apple's App Store authentication service.

Provenance: the tables were reverse-engineered and published by the
ApplePackage project (https://github.com/Lakr233/ApplePackage), MIT License
(see `LICENSE` in this directory). `rng-tables.bin` and
`f2-parameters.json` are mechanical re-encodings of that project's published
constant data into binary/JSON form. All algorithm code in this repository
that consumes the tables (`Core/src/sap_software.cpp`) is an independent
implementation written for this project.

`Scripts/generate_sap_tables.py` turns this directory into
`Core/src/sap_tables.cpp` / `Core/include/ipashelf/sap_tables.hpp`.
