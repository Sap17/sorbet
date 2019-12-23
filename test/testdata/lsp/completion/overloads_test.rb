# typed: true
# disable-stress-incremental: true

class I; end
class S; end
class A
  extend T::Sig
  sig {params(x: I).void}
  sig {params(x: S).void}
  def my_method(x); end
end

# Not possible to see in the test, but in VS Code though these items have the
# same name, they show up with different sigs.
#
# The order here doesn't particularly matter; if you're here because you made a
# change to the order that broke this test, feel free to update the test.

A.new.my_metho # error: does not exist
#             ^ completion: my_method, my_method
#             ^ apply-completion: [A] item: 0
#             ^ apply-completion: [B] item: 1
