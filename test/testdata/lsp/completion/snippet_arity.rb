# typed: true

class Test
  def test_method_nullary; end

  def test_method_unary(x); end

  def test_method_binary(x, y); end

  def test_method_kwarg(x:); end
end

Test.new.test_method_null # error: does not exist
#                        ^ apply-completion: [A] item: 0

Test.new.test_method_unar # error: does not exist
#                        ^ apply-completion: [B] item: 0

Test.new.test_method_bina # error: does not exist
#                        ^ apply-completion: [C] item: 0

Test.new.test_method_kwar # error: does not exist
#                        ^ apply-completion: [D] item: 0
