# typed: true

class FalseClass
  extend T::Sig

  sig {returns(TrueClass)}
  def blank?
    true
  end
end

class NilClass
  extend T::Sig

  sig {returns(TrueClass)}
  def blank?
    true
  end
end

class Object
  def blank?
<<<<<<< HEAD
    !self
=======
    !!self
>>>>>>> 1b7009d007f8ed838eb18a825cf15516871de77d
  end
end

class A
  extend T::Sig

  sig {params(s: T.nilable(String)).returns(NilClass)}
  def test_return(s)
    if !s.blank?
      s.length
      nil
    end
    nil
  end

  sig {params(s: String).returns(NilClass)}
  def test_unchanged(s)
    if s.blank?
      s.length
    else
      s.length
    end
    nil
  end

  def unreachable_nil()
<<<<<<< HEAD
    if !nil.blank?
      "foo" # error: This code is unreachable
    else
      "bar"
=======
    a = nil
    if !a.blank?
      puts a # error: This code is unreachable
>>>>>>> 1b7009d007f8ed838eb18a825cf15516871de77d
    end
  end

  def unreachable_false()
<<<<<<< HEAD
    if !false.blank?
      "foo" # error: This code is unreachable
    else
      "bar"
=======
    a = false
    if !a.blank?
      puts a # error: This code is unreachable
>>>>>>> 1b7009d007f8ed838eb18a825cf15516871de77d
    end
  end
end
