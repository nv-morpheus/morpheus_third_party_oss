<?php
# Generated by the protocol buffer compiler.  DO NOT EDIT!
# source: math.proto

namespace Math;

use Google\Protobuf\Internal\GPBType;
use Google\Protobuf\Internal\RepeatedField;
use Google\Protobuf\Internal\GPBUtil;

/**
 * Generated from protobuf message <code>math.Num</code>
 */
class Num extends \Google\Protobuf\Internal\Message
{
    /**
     * Generated from protobuf field <code>int64 num = 1;</code>
     */
    protected $num = 0;

    /**
     * Constructor.
     *
     * @param array $data {
     *     Optional. Data for populating the Message object.
     *
     *     @type int|string $num
     * }
     */
    public function __construct($data = NULL) {
        \GPBMetadata\Math::initOnce();
        parent::__construct($data);
    }

    /**
     * Generated from protobuf field <code>int64 num = 1;</code>
     * @return int|string
     */
    public function getNum()
    {
        return $this->num;
    }

    /**
     * Generated from protobuf field <code>int64 num = 1;</code>
     * @param int|string $var
     * @return $this
     */
    public function setNum($var)
    {
        GPBUtil::checkInt64($var);
        $this->num = $var;

        return $this;
    }

}

