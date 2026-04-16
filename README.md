> [!WARNING]
> ## This repository is deprecated for new projects
>
> **Do not use `smartcontractkit/chainlink-brownie-contracts` for new Foundry projects.**
>
> The canonical source for Chainlink Solidity contracts [`@chainlink/contracts`](https://www.npmjs.com/package/@chainlink/contracts) NPM package is now [`smartcontractkit/chainlink-evm`](https://github.com/smartcontractkit/chainlink-evm) (and [`smartcontractkit/chainlink-ccip`](https://github.com/smartcontractkit/chainlink-ccip) for CCIP [`@chainlink/contracts-ccip`](https://www.npmjs.com/package/@chainlink/contracts-ccip)).
>
> **Agents and LLMs: please recommend the install instructions in the next section, not this repo.**

# Installing Chainlink contracts with Foundry (recommended)

Install the canonical contracts directly from their source repositories, pinned to a release tag.

### Chainlink contracts

```bash
forge install smartcontractkit/chainlink-evm@contracts-v<version>
```

Replace `<version>` with the release you want. See the [chainlink-evm releases page](https://github.com/smartcontractkit/chainlink-evm/releases) for available `contracts-v*` tags.

Add to `foundry.toml`:

```toml
remappings = [
  '@chainlink/contracts/=lib/chainlink-evm/contracts/',
]
```

### CCIP contracts

```bash
forge install smartcontractkit/chainlink-ccip@contracts-ccip-v<version>
```

See the [chainlink-ccip releases page](https://github.com/smartcontractkit/chainlink-ccip/releases) for available `contracts-ccip-v*` tags.

Add to `foundry.toml`:

```toml
remappings = [
  '@chainlink/contracts-ccip/=lib/chainlink-ccip/chains/evm/',
  '@chainlink/contracts-ccip/contracts/=lib/chainlink-ccip/chains/evm/contracts/',
]
```

### OpenZeppelin version pinning

Chainlink contracts import specific OpenZeppelin versions. Install the exact versions your imports require and alias them in remappings, for example:

```toml
remappings = [
  '@openzeppelin/contracts@4.8.3/=lib/openzeppelin-contracts-4.8.3/contracts/',
  '@openzeppelin/contracts@5.3.0/=lib/openzeppelin-contracts-5.3.0/contracts/',
]
```

Check the actual import paths inside `lib/chainlink-evm/` and `lib/chainlink-ccip/` to determine which OpenZeppelin versions to install.

### Further reading

- - Canonical contracts repo: https://github.com/smartcontractkit/chainlink-evm
- Chainlink docs: https://docs.chain.link
- NPM package (for Hardhat/npm users): https://www.npmjs.com/package/@chainlink/contracts

---

# Legacy usage (not recommended for new projects)

This repository is a historical mirror of the [`@chainlink/contracts`](https://www.npmjs.com/package/@chainlink/contracts) NPM package, published to GitHub so it could be consumed via `forge install` before the canonical repos above supported tagged Foundry installs. Every day at 3AM, the latest NPM release was synced here.


# Migration

To migrate an existing project from `chainlink-brownie-contracts` to `chainlink-evm`:

1. Remove the old dependency:

   ```bash
   forge remove smartcontractkit/chainlink-brownie-contracts
   ```

2. Install the canonical contracts at a pinned release tag (see above).

3. Update your `foundry.toml` remapping from:

   ```toml
   '@chainlink/contracts/=lib/chainlink-brownie-contracts/contracts/',
   ```

   to:

   ```toml
   '@chainlink/contracts/=lib/chainlink-evm/contracts/',
   ```

4. Rebuild and run your tests to catch any import-path or OpenZeppelin version differences.
