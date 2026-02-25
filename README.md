# MDMX Tooling for Davinci Resolve

No warranty, in development. Use at your own risk.

CUDA is required.

## Usage

1. Download entire repository
2. Close Davinci Resolve.
3. Run installers for the tools you want located in the scripts folder.
4. Run davinci resolve.

To use the CRC calc, add the CRC calc effect to an adjustment clip above the lighting data clips. Adjust the size of the MDMX blade in the settings (usually 1920 px)

To use the fixture generator, add the fixture generator effect to your timeline. Load a fixture json file from the `mdmx-ofx-fixtures/fixtures` folder or make your own by referencing those files. More information on the json structure there can be found in the [source code](https://github.com/valuef/mdmx-davinci/blob/master/mdmx-ofx-fixtures/ofx.cpp) (search for `struct Fixture` and `parse_fixture_json`)
