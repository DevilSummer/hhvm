<?hh // strict
// Copyright 2004-present Facebook. All Rights Reserved.

class Cov<+T> { }

class C<+T> {
  public function foo<Tu>(Tu $x):T where Cov<T> super Cov<Tu> {
    return $x;
  }
}
