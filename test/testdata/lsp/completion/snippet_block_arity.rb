# typed: true

class Module
  include T::Sig
end

class A
  sig {params(blk: T.proc.params(x: Integer).void).void}
  def self.unary_block(&blk)
    yield 0
  end

  sig {params(blk: T.proc.params(x: Integer, y: String).void).void}
  def self.binary_block(&blk)
    yield 0, ''
  end

  sig do
    params(
      arg0: Integer,
      arg1: String,
      blk: T.proc.params(x: Integer, y: String).void
    )
    .void
  end
  def self.arg0_arg1_blk(arg0, arg1, &blk)
    yield arg0, arg1
  end
end

A.unary_bloc # error: does not exist
#           ^ apply-completion: [A] item: 0
A.binary_bloc # error: does not exist
#            ^ apply-completion: [B] item: 0
A.arg0_arg1_bl # error: does not exist
#             ^ apply-completion: [C] item: 0
