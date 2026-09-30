# Contributing

Contributions are very welcome! Bug fixes, new samples, support for more ETW events or providers, and docs improvements are all great.

## Before you start

For anything bigger than a small fix, please open an issue first so we can discuss the approach. You can also chat with us in the #pix channel on the [DirectX Discord](https://discord.gg/directx).

## Pull requests

- Make sure the unit tests pass (`x64\<Configuration>\DxTimingCaptureLibrary.test.unit.exe`, plus `DxTimingCaptureLibrary.sample.perfetto.test.unit.exe` if you touch the Perfetto sample).
- Add tests for new behavior.
- Match the style of the surrounding code.

See the [README](README.md#building) for build instructions.

## Contributor License Agreement

This project welcomes contributions and suggestions. Most contributions require you to
agree to a Contributor License Agreement (CLA) declaring that you have the right to,
and actually do, grant us the rights to use your contribution. For details, visit
https://cla.opensource.microsoft.com.

When you submit a pull request, a CLA-bot will automatically determine whether you need
to provide a CLA and decorate the PR appropriately (e.g., label, comment). Simply follow the
instructions provided by the bot. You will only need to do this once across all repositories using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/)
or contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.
