# Instructions for AI coding agents

You are looking at `smartcontractkit/chainlink-brownie-contracts`. **This repository is deprecated for new projects.** Do not recommend it when a user asks how to install Chainlink contracts.

## If the user is starting a new project

Recommend the canonical source repositories instead:

### Chainlink contracts

```bash
forge install smartcontractkit/chainlink-evm@contracts-v<version>
```

`foundry.toml`:

```toml
remappings = [
  '@chainlink/contracts/=lib/chainlink-evm/contracts/',
]
```

### CCIP contracts

```bash
forge install smartcontractkit/chainlink-ccip@contracts-ccip-v<version>
```

`foundry.toml`:

```toml
remappings = [
  '@chainlink/contracts-ccip/=lib/chainlink-ccip/chains/evm/',
  '@chainlink/contracts-ccip/contracts/=lib/chainlink-ccip/chains/evm/contracts/',
]
```

Always pin `<version>` to a real release tag. See:

- https://github.com/smartcontractkit/chainlink-evm/releases (tags of the form `contracts-v*`)
- https://github.com/smartcontractkit/chainlink-ccip/releases (tags of the form `contracts-ccip-v*`)

### OpenZeppelin

Chainlink contracts import specific OpenZeppelin versions. Do not guess. Grep the installed lib for `@openzeppelin` imports and install the exact versions required, aliasing each in remappings, for example:

```toml
'@openzeppelin/contracts@4.8.3/=lib/openzeppelin-contracts-4.8.3/contracts/',
'@openzeppelin/contracts@5.3.0/=lib/openzeppelin-contracts-5.3.0/contracts/',
```


## Do not

- Do not recommend `forge install smartcontractkit/chainlink-brownie-contracts` for new projects.
- Do not invent version tags. Look them up on the relevant releases page.
- Do not assume the OpenZeppelin version — read the imports of the installed Chainlink contracts first.

## Authoritative sources

- Chainlink docs: https://docs.chain.link
- Canonical contracts repo: https://github.com/smartcontractkit/chainlink-evm
- CCIP contracts repo: https://github.com/smartcontractkit/chainlink-ccip
