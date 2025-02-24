/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/TemporalFields.h"

#include "mozilla/Assertions.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/Range.h"
#include "mozilla/RangedPtr.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdint.h>
#include <utility>

#include "jsnum.h"
#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/temporal/Temporal.h"
#include "ds/Sort.h"
#include "gc/Barrier.h"
#include "gc/Tracer.h"
#include "js/AllocPolicy.h"
#include "js/ComparisonOperators.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/GCAPI.h"
#include "js/GCVector.h"
#include "js/Id.h"
#include "js/Printer.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"
#include "js/Value.h"
#include "util/Text.h"
#include "vm/BytecodeUtil.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"

#include "vm/JSAtomUtils-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

void TemporalFields::trace(JSTracer* trc) {
  TraceNullableRoot(trc, &monthCode, "TemporalFields::monthCode");
  TraceNullableRoot(trc, &offset, "TemporalFields::offset");
  TraceNullableRoot(trc, &era, "TemporalFields::era");
  TraceRoot(trc, &timeZone, "TemporalFields::timeZone");
}

static PropertyName* ToPropertyName(JSContext* cx, TemporalField field) {
  switch (field) {
    case TemporalField::Year:
      return cx->names().year;
    case TemporalField::Month:
      return cx->names().month;
    case TemporalField::MonthCode:
      return cx->names().monthCode;
    case TemporalField::Day:
      return cx->names().day;
    case TemporalField::Hour:
      return cx->names().hour;
    case TemporalField::Minute:
      return cx->names().minute;
    case TemporalField::Second:
      return cx->names().second;
    case TemporalField::Millisecond:
      return cx->names().millisecond;
    case TemporalField::Microsecond:
      return cx->names().microsecond;
    case TemporalField::Nanosecond:
      return cx->names().nanosecond;
    case TemporalField::Offset:
      return cx->names().offset;
    case TemporalField::Era:
      return cx->names().era;
    case TemporalField::EraYear:
      return cx->names().eraYear;
    case TemporalField::TimeZone:
      return cx->names().timeZone;
  }
  MOZ_CRASH("invalid temporal field name");
}

static const char* ToCString(TemporalField field) {
  switch (field) {
    case TemporalField::Year:
      return "year";
    case TemporalField::Month:
      return "month";
    case TemporalField::MonthCode:
      return "monthCode";
    case TemporalField::Day:
      return "day";
    case TemporalField::Hour:
      return "hour";
    case TemporalField::Minute:
      return "minute";
    case TemporalField::Second:
      return "second";
    case TemporalField::Millisecond:
      return "millisecond";
    case TemporalField::Microsecond:
      return "microsecond";
    case TemporalField::Nanosecond:
      return "nanosecond";
    case TemporalField::Offset:
      return "offset";
    case TemporalField::Era:
      return "era";
    case TemporalField::EraYear:
      return "eraYear";
    case TemporalField::TimeZone:
      return "timeZone";
  }
  MOZ_CRASH("invalid temporal field name");
}

static JS::UniqueChars QuoteString(JSContext* cx, const char* str) {
  Sprinter sprinter(cx);
  if (!sprinter.init()) {
    return nullptr;
  }
  mozilla::Range range(reinterpret_cast<const Latin1Char*>(str),
                       std::strlen(str));
  if (!QuoteString<QuoteTarget::String>(&sprinter, range)) {
    return nullptr;
  }
  return sprinter.release();
}

static JS::UniqueChars QuoteString(JSContext* cx, PropertyKey key) {
  if (key.isString()) {
    return QuoteString(cx, key.toString());
  }

  if (key.isInt()) {
    Int32ToCStringBuf buf;
    size_t length;
    const char* str = Int32ToCString(&buf, key.toInt(), &length);
    return DuplicateString(cx, str, length);
  }

  MOZ_ASSERT(key.isSymbol());
  return QuoteString(cx, key.toSymbol()->description());
}

static mozilla::Maybe<TemporalField> ToTemporalField(JSContext* cx,
                                                     PropertyKey property) {
  static constexpr TemporalField fieldNames[] = {
      TemporalField::Year,        TemporalField::Month,
      TemporalField::MonthCode,   TemporalField::Day,
      TemporalField::Hour,        TemporalField::Minute,
      TemporalField::Second,      TemporalField::Millisecond,
      TemporalField::Microsecond, TemporalField::Nanosecond,
      TemporalField::Offset,      TemporalField::Era,
      TemporalField::EraYear,     TemporalField::TimeZone,
  };

  for (const auto& fieldName : fieldNames) {
    auto* name = ToPropertyName(cx, fieldName);
    if (property.isAtom(name)) {
      return mozilla::Some(fieldName);
    }
  }
  return mozilla::Nothing();
}

static JSString* ToPrimitiveAndRequireString(JSContext* cx,
                                             Handle<Value> value) {
  Rooted<Value> primitive(cx, value);
  if (!ToPrimitive(cx, JSTYPE_STRING, &primitive)) {
    return nullptr;
  }
  if (!primitive.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, primitive,
                     nullptr, "not a string");
    return nullptr;
  }
  return primitive.toString();
}

static Value TemporalFieldDefaultValue(TemporalField field) {
  switch (field) {
    case TemporalField::Year:
    case TemporalField::Month:
    case TemporalField::MonthCode:
    case TemporalField::Day:
    case TemporalField::Offset:
    case TemporalField::Era:
    case TemporalField::EraYear:
    case TemporalField::TimeZone:
      return UndefinedValue();
    case TemporalField::Hour:
    case TemporalField::Minute:
    case TemporalField::Second:
    case TemporalField::Millisecond:
    case TemporalField::Microsecond:
    case TemporalField::Nanosecond:
      return Int32Value(0);
  }
  MOZ_CRASH("invalid temporal field name");
}

static bool TemporalFieldConvertValue(JSContext* cx, TemporalField field,
                                      MutableHandle<Value> value) {
  auto* name = ToCString(field);
  switch (field) {
    case TemporalField::Year:
    case TemporalField::Hour:
    case TemporalField::Minute:
    case TemporalField::Second:
    case TemporalField::Millisecond:
    case TemporalField::Microsecond:
    case TemporalField::Nanosecond:
    case TemporalField::EraYear: {
      double num;
      if (!ToIntegerWithTruncation(cx, value, name, &num)) {
        return false;
      }
      value.setNumber(num);
      return true;
    }

    case TemporalField::Month:
    case TemporalField::Day: {
      double num;
      if (!ToPositiveIntegerWithTruncation(cx, value, name, &num)) {
        return false;
      }
      value.setNumber(num);
      return true;
    }

    case TemporalField::MonthCode:
    case TemporalField::Offset:
    case TemporalField::Era: {
      JSString* str = ToPrimitiveAndRequireString(cx, value);
      if (!str) {
        return false;
      }
      value.setString(str);
      return true;
    }

    case TemporalField::TimeZone:
      // NB: timeZone has no conversion function.
      return true;
  }
  MOZ_CRASH("invalid temporal field name");
}

static int32_t ComparePropertyKey(PropertyKey x, PropertyKey y) {
  MOZ_ASSERT(x.isAtom() || x.isInt());
  MOZ_ASSERT(y.isAtom() || y.isInt());

  if (MOZ_LIKELY(x.isAtom() && y.isAtom())) {
    return CompareStrings(x.toAtom(), y.toAtom());
  }

  if (x.isInt() && y.isInt()) {
    return x.toInt() - y.toInt();
  }

  uint32_t index = uint32_t(x.isInt() ? x.toInt() : y.toInt());
  JSAtom* str = x.isAtom() ? x.toAtom() : y.toAtom();

  char16_t buf[UINT32_CHAR_BUFFER_LENGTH];
  mozilla::RangedPtr<char16_t> end(std::end(buf), buf, std::end(buf));
  mozilla::RangedPtr<char16_t> start = BackfillIndexInCharBuffer(index, end);

  int32_t result = CompareChars(start.get(), end - start, str);
  return x.isInt() ? result : -result;
}

#ifdef DEBUG
static bool IsSorted(std::initializer_list<TemporalField> fieldNames) {
  return std::is_sorted(fieldNames.begin(), fieldNames.end(),
                        [](auto x, auto y) {
                          auto* a = ToCString(x);
                          auto* b = ToCString(y);
                          return std::strcmp(a, b) < 0;
                        });
}

static bool IsSorted(const JS::StackGCVector<PropertyKey>& fieldNames) {
  return std::is_sorted(
      fieldNames.begin(), fieldNames.end(),
      [](auto x, auto y) { return ComparePropertyKey(x, y) < 0; });
}
#endif

// clang-format off
//
// TODO: |fields| is often a built-in Temporal type, so we likely want to
// optimise for this case.
//
// Consider the case when PlainDate.prototype.toPlainMonthDay is called. The
// following steps are applied:
//
// 1. CalendarFields(calendar, «"day", "monthCode"») is called to retrieve the
//    relevant calendar fields. For (most?) built-in calendars this will just
//    return the input list «"day", "monthCode"».
// 2. PrepareTemporalFields(plainDate, «"day", "monthCode"») is called. This
//    will access the properties `plainDate.day` and `plainDate.monthCode`.
//   a. `plainDate.day` will call CalendarDay(calendar, plainDate).
//   b. For built-in calendars, this will simply access `plainDate.[[IsoDay]]`.
//   c. `plainDate.monthCode` will call CalendarMonthCode(calendar, plainDate).
//   d. For built-in calendars, ISOMonthCode(plainDate.[[IsoMonth]]) is called.
// 3. CalendarMonthDayFromFields(calendar, {day, monthCode}) is called.
// 4. For built-in calendars, this calls PrepareTemporalFields({day, monthCode},
//    «"day", "month", "monthCode", "year"», «"day"»).
// 5. The previous PrepareTemporalFields call is a no-op and returns {day, monthCode}.
// 6. Then ISOMonthDayFromFields({day, monthCode}, "constrain") gets called.
// 7. ResolveISOMonth(monthCode) is called to parse the just created `monthCode`.
// 8. RegulateISODate(referenceISOYear, month, day, "constrain") is called.
// 9. Finally CreateTemporalMonthDay is called to create the PlainMonthDay instance.
//
// All these steps could be simplified to just:
// 1. CreateTemporalMonthDay(referenceISOYear, plainDate.[[IsoMonth]], plainDate.[[IsoDay]]).
//
// When the following conditions are true:
// 1. The `plainDate` is a Temporal.PlainDate instance and has no overridden methods.
// 2. The `calendar` is a Temporal.Calendar instance and has no overridden methods.
// 3. Temporal.PlainDate.prototype and Temporal.Calendar.prototype are in their initial state.
// 4. Array iteration is still in its initial state. (Required by CalendarFields)
//
// PlainDate_toPlainMonthDay has an example implementation for this optimisation.
//
// clang-format on

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields )
 */
bool js::temporal::PrepareTemporalFields(
    JSContext* cx, Handle<JSObject*> fields,
    std::initializer_list<TemporalField> fieldNames,
    std::initializer_list<TemporalField> requiredFields,
    MutableHandle<TemporalFields> result) {
  // Steps 1-3. (Not applicable in our implementation.)

  // Step 4. (|fieldNames| is sorted in our implementation.)
  MOZ_ASSERT(IsSorted(fieldNames));

  // Step 5. (The list doesn't contain duplicates in our implementation.)
  MOZ_ASSERT(std::adjacent_find(fieldNames.begin(), fieldNames.end()) ==
             fieldNames.end());

  // |requiredFields| is sorted and doesn't contain any duplicate elements.
  MOZ_ASSERT(IsSorted(requiredFields));
  MOZ_ASSERT(std::adjacent_find(requiredFields.begin(), requiredFields.end()) ==
             requiredFields.end());

  // Step 6.
  Rooted<Value> value(cx);
  for (auto fieldName : fieldNames) {
    auto* property = ToPropertyName(cx, fieldName);
    auto* cstr = ToCString(fieldName);

    // Step 6.a. (Not applicable in our implementation.)

    // Step 6.b.i.
    if (!GetProperty(cx, fields, fields, property, &value)) {
      return false;
    }

    // Steps 6.b.ii-iii.
    if (!value.isUndefined()) {
      // Step 6.b.ii.1. (Not applicable in our implementation.)

      // Steps 6.b.ii.2-3.
      switch (fieldName) {
        case TemporalField::Year:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.year())) {
            return false;
          }
          break;
        case TemporalField::Month:
          if (!ToPositiveIntegerWithTruncation(cx, value, cstr,
                                               &result.month())) {
            return false;
          }
          break;
        case TemporalField::MonthCode: {
          JSString* str = ToPrimitiveAndRequireString(cx, value);
          if (!str) {
            return false;
          }
          result.monthCode().set(str);
          break;
        }
        case TemporalField::Day:
          if (!ToPositiveIntegerWithTruncation(cx, value, cstr,
                                               &result.day())) {
            return false;
          }
          break;
        case TemporalField::Hour:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.hour())) {
            return false;
          }
          break;
        case TemporalField::Minute:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.minute())) {
            return false;
          }
          break;
        case TemporalField::Second:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.second())) {
            return false;
          }
          break;
        case TemporalField::Millisecond:
          if (!ToIntegerWithTruncation(cx, value, cstr,
                                       &result.millisecond())) {
            return false;
          }
          break;
        case TemporalField::Microsecond:
          if (!ToIntegerWithTruncation(cx, value, cstr,
                                       &result.microsecond())) {
            return false;
          }
          break;
        case TemporalField::Nanosecond:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.nanosecond())) {
            return false;
          }
          break;
        case TemporalField::Offset: {
          JSString* str = ToPrimitiveAndRequireString(cx, value);
          if (!str) {
            return false;
          }
          result.offset().set(str);
          break;
        }
        case TemporalField::Era: {
          JSString* str = ToPrimitiveAndRequireString(cx, value);
          if (!str) {
            return false;
          }
          result.era().set(str);
          break;
        }
        case TemporalField::EraYear:
          if (!ToIntegerWithTruncation(cx, value, cstr, &result.eraYear())) {
            return false;
          }
          break;
        case TemporalField::TimeZone:
          // NB: TemporalField::TimeZone has no conversion function.
          result.timeZone().set(value);
          break;
      }
    } else {
      // Step 6.b.iii.1.
      if (std::find(requiredFields.begin(), requiredFields.end(), fieldName) !=
          requiredFields.end()) {
        if (auto chars = QuoteString(cx, cstr)) {
          JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                    JSMSG_TEMPORAL_MISSING_PROPERTY,
                                    chars.get());
        }
        return false;
      }

      // `const` can be changed to `constexpr` when we switch to C++20.
      const TemporalFields FallbackValues{};

      // Steps 6.b.iii.2-3.
      switch (fieldName) {
        case TemporalField::Year:
          result.year() = FallbackValues.year;
          break;
        case TemporalField::Month:
          result.month() = FallbackValues.month;
          break;
        case TemporalField::MonthCode:
          result.monthCode().set(FallbackValues.monthCode);
          break;
        case TemporalField::Day:
          result.day() = FallbackValues.day;
          break;
        case TemporalField::Hour:
          result.hour() = FallbackValues.hour;
          break;
        case TemporalField::Minute:
          result.minute() = FallbackValues.minute;
          break;
        case TemporalField::Second:
          result.second() = FallbackValues.second;
          break;
        case TemporalField::Millisecond:
          result.millisecond() = FallbackValues.millisecond;
          break;
        case TemporalField::Microsecond:
          result.microsecond() = FallbackValues.microsecond;
          break;
        case TemporalField::Nanosecond:
          result.nanosecond() = FallbackValues.nanosecond;
          break;
        case TemporalField::Offset:
          result.offset().set(FallbackValues.offset);
          break;
        case TemporalField::Era:
          result.era().set(FallbackValues.era);
          break;
        case TemporalField::EraYear:
          result.eraYear() = FallbackValues.eraYear;
          break;
        case TemporalField::TimeZone:
          result.timeZone().set(FallbackValues.timeZone);
          break;
      }
    }

    // Steps 6.c-d. (Not applicable in our implementation.)
  }

  // Step 7. (Not applicable in our implementation.)

  // Step 8.
  return true;
}

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * duplicateBehaviour ] )
 */
PlainObject* js::temporal::PrepareTemporalFields(
    JSContext* cx, Handle<JSObject*> fields,
    Handle<JS::StackGCVector<PropertyKey>> fieldNames) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Rooted<PlainObject*> result(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!result) {
    return nullptr;
  }

  // Step 3. (Not applicable in our implementation.)

  // Step 4. (The list is already sorted in our implementation.)
  MOZ_ASSERT(IsSorted(fieldNames));

  // Step 5. (The list doesn't contain duplicates in our implementation.)
  MOZ_ASSERT(std::adjacent_find(fieldNames.begin(), fieldNames.end()) ==
             fieldNames.end());

  // Step 6.
  Rooted<Value> value(cx);
  for (size_t i = 0; i < fieldNames.length(); i++) {
    Handle<PropertyKey> property = fieldNames[i];

    // Step 6.a.
    MOZ_ASSERT(property != NameToId(cx->names().constructor));
    MOZ_ASSERT(property != NameToId(cx->names().proto_));

    // Step 6.b.i.
    if (!GetProperty(cx, fields, fields, property, &value)) {
      return nullptr;
    }

    // Steps 6.b.ii-iii.
    if (auto fieldName = ToTemporalField(cx, property)) {
      if (!value.isUndefined()) {
        // Step 6.b.ii.1. (Not applicable in our implementation.)

        // Step 6.b.ii.2.
        if (!TemporalFieldConvertValue(cx, *fieldName, &value)) {
          return nullptr;
        }
      } else {
        // Step 6.b.iii.1. (Not applicable in our implementation.)

        // Step 6.b.iii.2.
        value = TemporalFieldDefaultValue(*fieldName);
      }
    }

    // Steps 6.b.ii.3 and 6.b.iii.3.
    if (!DefineDataProperty(cx, result, property, value)) {
      return nullptr;
    }

    // Steps 6.c-d. (Not applicable in our implementation.)
  }

  // Step 7. (Not applicable in our implementation.)

  // Step 8.
  return result;
}

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * duplicateBehaviour ] )
 */
PlainObject* js::temporal::PrepareTemporalFields(
    JSContext* cx, Handle<JSObject*> fields,
    Handle<JS::StackGCVector<PropertyKey>> fieldNames,
    std::initializer_list<TemporalField> requiredFields) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Rooted<PlainObject*> result(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!result) {
    return nullptr;
  }

  // Step 3. (Not applicable in our implementation.)

  // Step 4. (The list is already sorted in our implementation.)
  MOZ_ASSERT(IsSorted(fieldNames));

  // Step 5. (The list doesn't contain duplicates in our implementation.)
  MOZ_ASSERT(std::adjacent_find(fieldNames.begin(), fieldNames.end()) ==
             fieldNames.end());

  // |requiredFields| is sorted and doesn't include any duplicate elements.
  MOZ_ASSERT(IsSorted(requiredFields));
  MOZ_ASSERT(std::adjacent_find(requiredFields.begin(), requiredFields.end()) ==
             requiredFields.end());

  // Step 6.
  Rooted<Value> value(cx);
  for (size_t i = 0; i < fieldNames.length(); i++) {
    Handle<PropertyKey> property = fieldNames[i];

    // Step 6.a.
    MOZ_ASSERT(property != NameToId(cx->names().constructor));
    MOZ_ASSERT(property != NameToId(cx->names().proto_));

    // Step 6.b.i.
    if (!GetProperty(cx, fields, fields, property, &value)) {
      return nullptr;
    }

    // Steps 6.b.ii-iii.
    if (auto fieldName = ToTemporalField(cx, property)) {
      if (!value.isUndefined()) {
        // Step 6.b.ii.1. (Not applicable in our implementation.)

        // Step 6.b.ii.2.
        if (!TemporalFieldConvertValue(cx, *fieldName, &value)) {
          return nullptr;
        }
      } else {
        // Step 6.b.iii.1.
        if (std::find(requiredFields.begin(), requiredFields.end(),
                      *fieldName) != requiredFields.end()) {
          if (auto chars = QuoteString(cx, property.toString())) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                      JSMSG_TEMPORAL_MISSING_PROPERTY,
                                      chars.get());
          }
          return nullptr;
        }

        // Step 6.b.iii.2.
        value = TemporalFieldDefaultValue(*fieldName);
      }
    }

    // Steps 6.b.ii.3 and 6.b.iii.3.
    if (!DefineDataProperty(cx, result, property, value)) {
      return nullptr;
    }

    // Steps 6.c-d. (Not applicable in our implementation.)
  }

  // Step 7. (Not applicable in our implementation.)

  // Step 8.
  return result;
}

/**
 * PrepareTemporalFields ( fields, fieldNames, requiredFields [ ,
 * duplicateBehaviour ] )
 */
PlainObject* js::temporal::PreparePartialTemporalFields(
    JSContext* cx, Handle<JSObject*> fields,
    Handle<JS::StackGCVector<PropertyKey>> fieldNames) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Rooted<PlainObject*> result(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!result) {
    return nullptr;
  }

  // Step 3.
  bool any = false;

  // Step 4. (The list is already sorted in our implementation.)
  MOZ_ASSERT(IsSorted(fieldNames));

  // Step 5. (The list doesn't contain duplicates in our implementation.)
  MOZ_ASSERT(std::adjacent_find(fieldNames.begin(), fieldNames.end()) ==
             fieldNames.end());

  // Step 6.
  Rooted<Value> value(cx);
  for (size_t i = 0; i < fieldNames.length(); i++) {
    Handle<PropertyKey> property = fieldNames[i];

    // Step 6.a.
    MOZ_ASSERT(property != NameToId(cx->names().constructor));
    MOZ_ASSERT(property != NameToId(cx->names().proto_));

    // Step 6.b.i.
    if (!GetProperty(cx, fields, fields, property, &value)) {
      return nullptr;
    }

    // Steps 6.b.ii-iii.
    if (!value.isUndefined()) {
      // Step 6.b.ii.1.
      any = true;

      // Step 6.b.ii.2.
      if (auto fieldName = ToTemporalField(cx, property)) {
        if (!TemporalFieldConvertValue(cx, *fieldName, &value)) {
          return nullptr;
        }
      }

      // Steps 6.b.ii.3.
      if (!DefineDataProperty(cx, result, property, value)) {
        return nullptr;
      }
    } else {
      // Step 6.b.iii. (Not applicable in our implementation.)
    }

    // Steps 6.c-d. (Not applicable in our implementation.)
  }

  // Step 7.
  if (!any) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_MISSING_TEMPORAL_FIELDS);
    return nullptr;
  }

  // Step 8.
  return result;
}

/**
 * Performs list-concatenation, removes any duplicates, and sorts the result.
 */
bool js::temporal::ConcatTemporalFieldNames(
    const JS::StackGCVector<PropertyKey>& receiverFieldNames,
    const JS::StackGCVector<PropertyKey>& inputFieldNames,
    JS::StackGCVector<PropertyKey>& concatenatedFieldNames) {
  MOZ_ASSERT(IsSorted(receiverFieldNames));
  MOZ_ASSERT(IsSorted(inputFieldNames));
  MOZ_ASSERT(concatenatedFieldNames.empty());

  auto appendUnique = [&](auto key) {
    if (concatenatedFieldNames.empty() ||
        concatenatedFieldNames.back() != key) {
      return concatenatedFieldNames.append(key);
    }
    return true;
  };

  size_t i = 0;
  size_t j = 0;

  // Append the names from |receiverFieldNames| and |inputFieldNames|.
  while (i < receiverFieldNames.length() && j < inputFieldNames.length()) {
    auto x = receiverFieldNames[i];
    auto y = inputFieldNames[j];

    PropertyKey z;
    if (ComparePropertyKey(x, y) <= 0) {
      z = x;
      i++;
    } else {
      z = y;
      j++;
    }
    if (!appendUnique(z)) {
      return false;
    }
  }

  // Append the remaining names from |receiverFieldNames|.
  while (i < receiverFieldNames.length()) {
    if (!appendUnique(receiverFieldNames[i++])) {
      return false;
    }
  }

  // Append the remaining names from |inputFieldNames|.
  while (j < inputFieldNames.length()) {
    if (!appendUnique(inputFieldNames[j++])) {
      return false;
    }
  }

  return true;
}

static auto* LowerBound(PropertyKey* begin, PropertyKey* end, PropertyKey key) {
  // Tell the analysis the |std::lower_bound| function can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  return std::lower_bound(begin, end, key, [](auto x, auto y) {
    return ComparePropertyKey(x, y) < 0;
  });
}

[[nodiscard]] static bool AppendSorted(
    JSContext* cx, JS::StackGCVector<PropertyKey>& fieldNames,
    PropertyKey additionalName) {
  // Find the position where to add |additionalName|.
  auto* p = LowerBound(fieldNames.begin(), fieldNames.end(), additionalName);

  // Reject duplicates per PrepareTemporalFields, step 6.c.
  if (p != fieldNames.end() && *p == additionalName) {
    if (auto chars = QuoteString(cx, additionalName)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_DUPLICATE_PROPERTY, chars.get());
    }
    return false;
  }

  // Store the index, because growBy() may reallocate, which invalidates |p|.
  size_t index = std::distance(fieldNames.begin(), p);

  // Allocate space for |additionalName|.
  if (!fieldNames.growBy(1)) {
    return false;
  }

  // Shift all entries starting at |index| to the right.
  std::copy_backward(fieldNames.begin() + index, fieldNames.end() - 1,
                     fieldNames.end());

  // Insert |additionalName|.
  fieldNames[index] = additionalName;
  return true;
}

[[nodiscard]] static bool AppendSorted(
    JSContext* cx, JS::StackGCVector<PropertyKey>& fieldNames,
    PropertyKey additionalNameOne, PropertyKey additionalNameTwo) {
  MOZ_ASSERT(ComparePropertyKey(additionalNameOne, additionalNameTwo) < 0);

  // Find the position where to add |additionalNameOne|.
  auto* p = LowerBound(fieldNames.begin(), fieldNames.end(), additionalNameOne);

  // |additionalNameTwo| can't occur before |p|.
  auto* q = LowerBound(p, fieldNames.end(), additionalNameTwo);

  // Reject duplicates per PrepareTemporalFields, step 6.c.
  if (p != fieldNames.end() && *p == additionalNameOne) {
    if (auto chars = QuoteString(cx, additionalNameOne)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_DUPLICATE_PROPERTY, chars.get());
    }
    return false;
  }

  // Reject duplicates per PrepareTemporalFields, step 6.c.
  if (q != fieldNames.end() && *q == additionalNameTwo) {
    if (auto chars = QuoteString(cx, additionalNameTwo)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_DUPLICATE_PROPERTY, chars.get());
    }
    return false;
  }

  // Store the indices, because growBy() may reallocate.
  size_t indexOne = std::distance(fieldNames.begin(), p);
  size_t indexTwo = std::distance(fieldNames.begin(), q);

  // Allocate space for both names.
  if (!fieldNames.growBy(2)) {
    return false;
  }

  // Shift all entries starting at |indexTwo| to the right.
  std::copy_backward(fieldNames.begin() + indexTwo, fieldNames.end() - 2,
                     fieldNames.end());

  // Shift all entries starting at |indexOne| to the right.
  std::copy_backward(fieldNames.begin() + indexOne,
                     fieldNames.begin() + indexTwo,
                     fieldNames.begin() + indexTwo + 1);

  // Insert both names.
  fieldNames[indexOne] = additionalNameOne;
  fieldNames[indexTwo + 1] = additionalNameTwo;
  return true;
}

bool js::temporal::AppendSorted(
    JSContext* cx, JS::StackGCVector<PropertyKey>& fieldNames,
    std::initializer_list<TemporalField> additionalNames) {
  // |fieldNames| is sorted and doesn't include any duplicates
  MOZ_ASSERT(IsSorted(fieldNames));
  MOZ_ASSERT(std::adjacent_find(fieldNames.begin(), fieldNames.end()) ==
             fieldNames.end());

  // |additionalNames| is non-empty, sorted, and doesn't include any duplicates.
  MOZ_ASSERT(additionalNames.size() > 0);
  MOZ_ASSERT(IsSorted(additionalNames));
  MOZ_ASSERT(
      std::adjacent_find(additionalNames.begin(), additionalNames.end()) ==
      additionalNames.end());

  if (additionalNames.size() == 1) {
    auto* it = additionalNames.begin();
    auto name = NameToId(ToPropertyName(cx, *it));
    return ::AppendSorted(cx, fieldNames, name);
  }

  if (additionalNames.size() == 2) {
    auto* it = additionalNames.begin();
    auto one = NameToId(ToPropertyName(cx, *it));
    auto two = NameToId(ToPropertyName(cx, *std::next(it)));
    return ::AppendSorted(cx, fieldNames, one, two);
  }

  // TODO: We can probably remove this general approach at a later time, because
  // only exactly one or two items are ever appended.

  // Allocate space for entries from |additionalNames|.
  if (!fieldNames.growBy(additionalNames.size())) {
    return false;
  }

  auto* left = std::prev(fieldNames.end(), additionalNames.size());
  auto* right = additionalNames.end();
  auto* out = fieldNames.end();

  // Write backwards into the newly allocated space.
  while (left != fieldNames.begin() && right != additionalNames.begin()) {
    MOZ_ASSERT(out != fieldNames.begin());
    auto x = *std::prev(left);
    auto y = NameToId(ToPropertyName(cx, *std::prev(right)));

    int32_t r = ComparePropertyKey(x, y);

    // Reject duplicates per PrepareTemporalFields, step 6.c.
    if (r == 0) {
      if (auto chars = QuoteString(cx, x)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TEMPORAL_DUPLICATE_PROPERTY,
                                  chars.get());
      }
      return false;
    }

    // Insert the lexicographically greater key.
    PropertyKey z;
    if (r > 0) {
      z = x;
      left--;
    } else {
      z = y;
      right--;
    }
    *--out = z;
  }

  // Avoid unnecessary copying if possible.
  if (left == out) {
    MOZ_ASSERT(right == additionalNames.begin());
    return true;
  }

  // Prepend the remaining names from |fieldNames|.
  while (left != fieldNames.begin()) {
    MOZ_ASSERT(out != fieldNames.begin());
    *--out = *--left;
  }

  // Prepend the remaining names from |additionalNames|.
  while (right != additionalNames.begin()) {
    MOZ_ASSERT(out != fieldNames.begin());
    *--out = NameToId(ToPropertyName(cx, *--right));
  }

  // All field names were written into the result list.
  MOZ_ASSERT(out == fieldNames.begin());

  return true;
}

bool js::temporal::SortTemporalFieldNames(
    JSContext* cx, JS::StackGCVector<PropertyKey>& fieldNames) {
  // Create scratch space for MergeSort().
  JS::StackGCVector<PropertyKey> scratch(cx);
  if (!scratch.resize(fieldNames.length())) {
    return false;
  }

  // Sort all field names in alphabetical order.
  auto comparator = [](const auto& x, const auto& y, bool* lessOrEqual) {
    *lessOrEqual = ComparePropertyKey(x, y) <= 0;
    return true;
  };
  MOZ_ALWAYS_TRUE(MergeSort(fieldNames.begin(), fieldNames.length(),
                            scratch.begin(), comparator));

  for (size_t i = 0; i < fieldNames.length(); i++) {
    auto property = fieldNames[i];

    // Reject "constructor" and "__proto__" per PrepareTemporalFields, step 6.a.
    if (property == NameToId(cx->names().constructor) ||
        property == NameToId(cx->names().proto_)) {
      if (auto chars = QuoteString(cx, property)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TEMPORAL_INVALID_PROPERTY, chars.get());
      }
      return false;
    }

    // Reject duplicates per PrepareTemporalFields, step 6.c.
    if (i > 0 && property == fieldNames[i - 1]) {
      if (auto chars = QuoteString(cx, property)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_TEMPORAL_DUPLICATE_PROPERTY,
                                  chars.get());
      }
      return false;
    }
  }

  return true;
}
