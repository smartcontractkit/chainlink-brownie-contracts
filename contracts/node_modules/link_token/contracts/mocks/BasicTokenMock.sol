pragma solidity ^0.4.8;


import '../token/linkBasicToken.sol';


// mock class using BasicToken
contract BasicTokenMock is linkBasicToken {

  function BasicTokenMock(address initialAccount, uint initialBalance)
  {
    balances[initialAccount] = initialBalance;
    totalSupply = initialBalance;
  }

}
