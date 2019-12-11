# typed: true
class Foo
  extend T::Sig

  sig { params(x: Integer).void }
  def initialize(x)
    @x = x  # this will get a synthetic `T.let`
    @y = x + 1  # This should not, because the RHS is not a simple local
  end

  sig { void }
  def foo
    T.reveal_type(@x) # error: Revealed type: `Integer`
    T.reveal_type(@y) # error: Revealed type: `T.untyped`
  end
end

class Bar
  extend T::Sig

  # we should find `params` correctly here
  sig { overridable.params(x: Integer).overridable.void }
  def initialize(x)
    @x = x
  end

  sig { void }
  def foo
    T.reveal_type(@x) # error: Revealed type: `Integer`
  end
end

class Baz
  extend T::Sig

  # nobody should ever write this, but just in case, we should make
  # sure not to include type_parameter types in a generated `T.let`
  sig { type_parameters(:U).params(x: T.type_parameter(:U)).void }
  def initialize(x)
    @x = x
  end

  sig { void }
  def foo
    T.reveal_type(@x) # error: Revealed type: `T.untyped`
  end
end


# Some miscellaneous weirder cases
class InterspersedExprs
  extend T::Sig

  sig { params(x: Integer).void }
  def initialize(x)
    ["hello", "world"].each do |w|
      puts w
    end
    @x = x
    puts "goodbye"
  end

  sig { void }
  def foo
    T.reveal_type(@x) # error: Revealed type: `Integer`
  end
end

# Some miscellaneous other cases
class InterspersedStmts
  extend T::Sig

  sig { params(x: Integer).void }
  def initialize(x)
    ["hello", "world"].each do |w|
      puts w
    end
    @x = x
    puts "goodbye"
  end

  sig { void }
  def foo
    T.reveal_type(@x) # error: Revealed type: `Integer`
  end
end

# Some less straightforward situations with arguments
class DifferentArgs
  extend T::Sig

  sig { params(x: Integer, y: T.nilable(String), z: T.any(T::Array[T::Boolean], T.proc.void)).void }
  def initialize(x, y, z)
    @a = y  # it works out-of-order
    r = x
    @b = r  # it does NOT work with a variable that's not explicitly a param
    @c = y  # a single param can be used with multiple instance variables
    [1, 2].each do
      @d = z  # it does NOT work within blocks
    end
    if true
      @e = z  # it does NOT work within other control flow
    end

    @f = z  # it does work with more sophisticated types
  end

  sig { void }
  def foo
    T.reveal_type(@a)  # error: Revealed type: `T.nilable(String)`
    T.reveal_type(@b)  # error: Revealed type: `T.untyped`
    T.reveal_type(@c)  # error: Revealed type: `T.nilable(String)
    T.reveal_type(@d)  # error: Revealed type: `T.untyped`
    T.reveal_type(@e)  # error: Revealed type: `T.untyped`
    T.reveal_type(@f)  # error: Revealed type: `T.any(T::Array[T::Boolean], T.proc.void)`
  end
end

class ClassVar
  extend T::Sig
  sig { params(x: Integer).void }
  def initialize(x)
    @@a = x  # it does NOT currently work with class variables
  end

  sig { void }
  def foo
    T.reveal_type(@@a)  # error: Revealed type: `T.untyped`
  end
end

class NestedConstructor
  extend T::Sig

  sig {void}
  def self.foo
    sig {params(x: Integer).void}
    def initialize(x)
      # it does not work if the constructor is not declared as a
      # top-level method of the class (i.e. if it is nested inside
      # something else) due to limitations about how we associate sigs
      @x = x
    end
  end

  def foo
    T.reveal_type(@x)  # error: Revealed type: `T.untyped`
  end
end
