pragma solidity ^0.4.8;


import '../token/linkStandardToken.sol';


contract StandardTokenMock is linkStandardToken {

  function StandardTokenMock(address initialAccount, uint initialBalance)
  {
    balances[initialAccount] = initialBalance;
    totalSupply = initialBalance;
  }

}
