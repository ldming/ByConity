#pragma once

#include <DataTypes/Serializations/SerializationDecimalBase.h>

class DateLUTImpl;

namespace DB
{

class SerializationDateTime64 final : public SerializationDecimalBase<DateTime64>
{
private:
    const DateLUTImpl & time_zone;
    const DateLUTImpl & utc_time_zone;

public:
    SerializationDateTime64(const DateLUTImpl & time_zone_, const DateLUTImpl & utc_time_zone_, UInt32 scale_);

    void checkDateOverflow(DateTime64 & x, const FormatSettings & settings) const;

    void serializeText(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeText(IColumn & column, ReadBuffer & istr, const FormatSettings &) const override;
    void deserializeWholeText(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const override;
    void serializeTextEscaped(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeTextEscaped(IColumn & column, ReadBuffer & istr, const FormatSettings &) const override;
    void serializeTextQuoted(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeTextQuoted(IColumn & column, ReadBuffer & istr, const FormatSettings &) const override;
    void serializeTextJSON(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeTextJSON(IColumn & column, ReadBuffer & istr, const FormatSettings &) const override;
    void serializeTextCSV(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeTextCSV(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const override;
};

}
