
# Overview
This repository is a slimmed down version of Chainlink's official repo. It clones *only* the Chainlink `contracts` folder and the repo automatically updates every time there is a new NPM release. 

- NPM's latest release can be found here: https://www.npmjs.com/package/@chainlink/contracts
- Chainlink's official repo: https://github.com/smartcontractkit/chainlink

# chainlink-brownie-contracts

A minimal repo that is a copy of the npm package [@chainlink/contracts](https://www.npmjs.com/package/@chainlink/contracts). These contracts are taken from the [core chainlink github](https://github.com/smartcontractkit/chainlink), compressed, and deployed to npm. 

Everyday at 3AM, the latest version of the package is updated here, this way, you can use the Chainlink contracts with foundry without having to use npm/yarn. This also makes other third party packages like Brownie and Ape easier to work with. 

## Usage

### Foundry

1. Run this in your projects root directory.

```bash
forge install smartcontractkit/chainlink-brownie-contracts --no-commit
```

2. Then, update your `foundry.toml` to include the following in the `remappings`.

```
remappings = [
  '@chainlink/contracts/=lib/chainlink-brownie-contracts/contracts/src/',
]
```

>! IMPORTANT
There were compatibility issues with `1.0.0`, `1.1.0` and `1.2.0`, where there were originally 2 versions of each version. We have deleted the deprecated and outdated versions so that there is no longer any conflict. 

All the releases of this package are going to match the [@chainlink/contracts NPM tags](https://www.npmjs.com/package/@chainlink/contracts). 
So the versioning will look "backwards", but we are starting with v0.2.1