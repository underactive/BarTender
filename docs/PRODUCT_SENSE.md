# {{PROJECT_NAME}} — Product Sense

This doc captures taste and product judgment. When making decisions that
aren't covered by a spec, use these principles.

## Who is the user?

<!-- Describe your target user in 2-3 sentences. What do they care about?
     What are they not interested in? What is their level of expertise? -->

## Product principles

### 1. Signal over noise
Prioritize meaningful output over volume. Users want curated, actionable
information — not a raw dump of everything the system knows. Filter,
deduplicate, and rank before presenting.

### 2. Zero-config by default
The default path should require no configuration. Sensible defaults for
everything. Power users can override, but the first experience should be:
point at input, get useful output.

### 3. Incremental, not ambitious
Ship the smallest useful thing. A working tool with one feature is more
valuable today than a platform with ten features that's half-built.

### 4. Respect human attention
Don't surface everything — surface what matters. Categorize by importance,
deduplicate across sources, and let the user drill down if they want more.

### 5. External input is untrusted
Third-party data, user input, and external service responses can be
malformed, unexpected, or adversarial. The product must handle all of this
gracefully — never crash on bad input, always surface confidence levels.
