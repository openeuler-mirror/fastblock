package osd

type ERRNUM int32

const (
	SUCCESS                     ERRNUM = 0
	OSD_ERR_NOT_APPLY                  = -156
	OSD_ERR_ID_CONFLICT                = -157
	OSD_ERR_ADDRESS_INVALID            = -158
	OSD_ERR_UPDATE_STATE_FAILED        = -159
)

type ErrorCode int

const EFailureDomainNeedNotSatisfied ErrorCode = 1
const EPoolAlreadyExists ErrorCode = 2
const EInternalError ErrorCode = 3
const EPoolNotExist ErrorCode = 4
const EPoolNotInstance ErrorCode = 5
const EOsdTreeNotExist ErrorCode = 6
const ENoEnoughOsd ErrorCode = 7
const EPgDistributionError ErrorCode = 8
const EUuidAlreadyExists ErrorCode = 9

func (e ErrorCode) Error() string {
	switch e {
	case EFailureDomainNeedNotSatisfied:
		return "failed domain need not satisfied, maybe the failure domain(osd、host、rack) is not enough"
	case EPoolAlreadyExists:
		return "the pool name is already occupied by other pools"
	case EInternalError:
		return "internal error, maybe etcd put/get error or marshal/unmarshal error"
	case EPoolNotExist:
		return "the pool is not exist"
	case EPoolNotInstance:
		return "pool instance is not exist"
	case EOsdTreeNotExist:
		return "osd tree instance is not exist"
	case ENoEnoughOsd:
		return "no enough osd to create pool(form quorum)"
	case EPgDistributionError:
		return "failed to distribute pg to osds, the calculation is error"
	case EUuidAlreadyExists:
		return "uuid is occupied by other osds"
	default:
		return "Unknown error"
	}
}